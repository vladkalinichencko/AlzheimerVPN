#include "gatewayController.h"

#include <algorithm>
#include <functional>
#include <random>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPromise>
#include <QScopedPointer>
#include <QSslError>
#include <QThread>
#include <QUrl>

#include "QBlockCipher.h"
#include "QRsa.h"

#include "amneziaApplication.h"
#include "core/utils/api/apiUtils.h"
#include "core/utils/constants/apiKeys.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/utilities.h"

#ifdef AMNEZIA_DESKTOP
    #include "core/utils/ipcClient.h"
#endif

namespace
{
    constexpr QLatin1String errorResponsePattern1("No active configuration found for");
    constexpr QLatin1String errorResponsePattern2("No non-revoked public key found for");
    constexpr QLatin1String errorResponsePattern3("Account not found.");

    constexpr QLatin1String updateRequestResponsePattern("client version update is required");

    constexpr int httpStatusCodeNotFound = 404;
    constexpr int httpStatusCodeConflict = 409;
    constexpr int httpStatusCodeNotImplemented = 501;
    constexpr int httpStatusCodePaymentRequired = 402;
    constexpr int httpStatusCodeUnprocessableEntity = 422;

    constexpr QLatin1String unprocessableSubscriptionMessage("Failed to retrieve subscription information. Is it activated?");

    constexpr int proxyStorageRequestTimeoutMsecs = 3000;

    QByteArray agwPublicKey(bool isDevEnvironment)
    {
        QByteArray key = isDevEnvironment ? DEV_AGW_PUBLIC_KEY : PROD_AGW_PUBLIC_KEY;
        return key.replace("\\n", "\n");
    }

    QNetworkAccessManager *networkManagerForCurrentThread()
    {
        QNetworkAccessManager *appNetworkManager = amnApp->networkManager();
        if (appNetworkManager->thread() == QThread::currentThread()) {
            return appNetworkManager;
        }

        static thread_local QScopedPointer<QNetworkAccessManager> threadNetworkManager;
        if (threadNetworkManager.isNull()) {
            threadNetworkManager.reset(new QNetworkAccessManager);
        }
        return threadNetworkManager.data();
    }

    void logGatewayRequest(const QString &endpoint, const QNetworkRequest &request, const QJsonObject &apiPayload, int timeoutMsecs)
    {
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_request"
            << "endpoint_template=" + endpoint
            << "url_host=" + request.url().host()
            << "timeout_ms=" + QString::number(timeoutMsecs)
            << "service_type=" + apiPayload.value(apiDefs::key::serviceType).toString()
            << "user_country=" + apiPayload.value(apiDefs::key::userCountryCode).toString()
            << "server_country=" + apiPayload.value(apiDefs::key::serverCountryCode).toString()
            << "protocol=" + apiPayload.value(apiDefs::key::protocol).toString()
            << "service_protocol=" + apiPayload.value(apiDefs::key::serviceProtocol).toString()
            << "has_uuid=" + QString(apiPayload.contains(apiDefs::key::uuid) ? "true" : "false")
            << "has_api_key=" + QString(apiPayload.contains(apiDefs::key::apiKey) ? "true" : "false")
            << "is_connect_event=" + QString(apiPayload.value(apiDefs::key::isConnectEvent).toBool(false) ? "true" : "false");
    }

    QString sslErrorsText(const QList<QSslError> &sslErrors)
    {
        QStringList errors;
        for (const QSslError &error : sslErrors) {
            errors << error.errorString();
        }
        return errors.join(QStringLiteral(" | ")).left(500);
    }

    void logGatewayPrepared(const QString &endpoint, const QNetworkRequest &request, int requestBytes)
    {
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_request_prepared"
            << "endpoint_template=" + endpoint
            << "url_host=" + request.url().host()
            << "url_path=" + request.url().path()
            << "request_bytes=" + QString::number(requestBytes)
            << "request_id=" + QString::fromUtf8(request.rawHeader("X-Client-Request-ID"));
    }

    void logGatewayResponse(const QString &stage, const QString &endpoint, const QUrl &url,
                            QNetworkReply::NetworkError replyError, const QString &replyErrorString, int httpStatusCode,
                            const QList<QSslError> &sslErrors, int encryptedBytes, bool decrypted, int decryptedBytes)
    {
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_response"
            << "stage=" + stage
            << "endpoint_template=" + endpoint
            << "url_host=" + url.host()
            << "url_path=" + url.path()
            << "http_status=" + QString::number(httpStatusCode)
            << "reply_error=" + QString::number(static_cast<int>(replyError))
            << "reply_error_text=" + replyErrorString
            << "ssl_error_count=" + QString::number(sslErrors.size())
            << "ssl_errors=" + sslErrorsText(sslErrors)
            << "encrypted_bytes=" + QString::number(encryptedBytes)
            << "decrypted=" + QString(decrypted ? "true" : "false")
            << "decrypted_bytes=" + QString::number(decryptedBytes);
    }

    QStringList buildProxyStorageUrls(bool isDevEnvironment, const QString &serviceType, const QString &userCountryCode)
    {
        QStringList primaryBaseUrls;
        QStringList fallbackBaseUrls;
        if (isDevEnvironment) {
            primaryBaseUrls = QString(DEV_S3_ENDPOINT).split(", ", Qt::SkipEmptyParts);
        } else {
            primaryBaseUrls = QString(PROD_S3_ENDPOINT).split(", ", Qt::SkipEmptyParts);
            fallbackBaseUrls = QString(FALLBACK_S3_ENDPOINT).split(", ", Qt::SkipEmptyParts);
        }

        std::random_device randomDevice;
        std::mt19937 generator(randomDevice());
        std::shuffle(primaryBaseUrls.begin(), primaryBaseUrls.end(), generator);
        std::shuffle(fallbackBaseUrls.begin(), fallbackBaseUrls.end(), generator);

        auto appendStorageUrls = [&serviceType, &userCountryCode](const QStringList &baseUrls, QStringList &target) {
            if (!serviceType.isEmpty()) {
                for (const auto &baseUrl : baseUrls) {
                    QByteArray path = ("endpoints-" + serviceType + "-" + userCountryCode).toUtf8();
                    target.push_back(baseUrl + path.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) + ".json");
                }
            }
            for (const auto &baseUrl : baseUrls) {
                target.push_back(baseUrl + "endpoints.json");
            }
        };

        QStringList proxyStorageUrls;
        appendStorageUrls(primaryBaseUrls, proxyStorageUrls);
        appendStorageUrls(fallbackBaseUrls, proxyStorageUrls);
        return proxyStorageUrls;
    }

}

GatewayController::GatewayController(const QString &gatewayEndpoint, const bool isDevEnvironment, const int requestTimeoutMsecs,
                                     const bool isStrictKillSwitchEnabled, QObject *parent)
    : QObject(parent),
      m_gatewayEndpoint(gatewayEndpoint),
      m_isDevEnvironment(isDevEnvironment),
      m_requestTimeoutMsecs(requestTimeoutMsecs),
      m_isStrictKillSwitchEnabled(isStrictKillSwitchEnabled)
{
}

GatewayController::EncryptedRequestData GatewayController::prepareRequest(const QString &endpoint, const QJsonObject &apiPayload)
{
    EncryptedRequestData encRequestData;
    encRequestData.errorCode = ErrorCode::NoError;

#ifdef Q_OS_IOS
    IosController::Instance()->requestInetAccess();
    QThread::msleep(10);
#endif

    encRequestData.request.setTransferTimeout(m_requestTimeoutMsecs);
    encRequestData.request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    encRequestData.request.setRawHeader(QString("X-Client-Request-ID").toUtf8(), QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    // Build the direct gateway request. post()/postAsync() may try gateway
    // proxy endpoints first, but this request remains the final fallback.
    encRequestData.request.setUrl(endpoint.arg(m_gatewayEndpoint));

    // bypass killSwitch exceptions for API-gateway
#ifdef AMNEZIA_DESKTOP
    if (m_isStrictKillSwitchEnabled) {
        QString host = QUrl(encRequestData.request.url()).host();
        QString ip = NetworkUtilities::getIPAddress(host);
        if (!ip.isEmpty()) {
            IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
                QRemoteObjectPendingReply<bool> reply = iface->addKillSwitchAllowedRange(QStringList { ip });
                if (!reply.waitForFinished(1000) || !reply.returnValue())
                    qWarning() << "GatewayController::prepareRequest(): Failed to execute remote addKillSwitchAllowedRange call";
            });
        }
    }
#endif

    QSimpleCrypto::QBlockCipher blockCipher;
    encRequestData.key = blockCipher.generatePrivateSalt(32);
    encRequestData.iv = blockCipher.generatePrivateSalt(32);
    encRequestData.salt = blockCipher.generatePrivateSalt(8);

    QJsonObject keyPayload;
    keyPayload[apiDefs::key::aesKey] = QString(encRequestData.key.toBase64());
    keyPayload[apiDefs::key::aesIv] = QString(encRequestData.iv.toBase64());
    keyPayload[apiDefs::key::aesSalt] = QString(encRequestData.salt.toBase64());

    QByteArray encryptedKeyPayload;
    QByteArray encryptedApiPayload;
    try {
        QSimpleCrypto::QRsa rsa;

        EVP_PKEY *publicKey = nullptr;
        try {
            QByteArray rsaKey = agwPublicKey(m_isDevEnvironment);
            QSimpleCrypto::QRsa rsa;
            publicKey = rsa.getPublicKeyFromByteArray(rsaKey);
        } catch (...) {
            Utils::logException();
            qCritical() << "error loading public key from environment variables";
            encRequestData.errorCode = ErrorCode::ApiMissingAgwPublicKey;
            return encRequestData;
        }

        encryptedKeyPayload = rsa.encrypt(QJsonDocument(keyPayload).toJson(), publicKey, RSA_PKCS1_PADDING);
        EVP_PKEY_free(publicKey);

        encryptedApiPayload = blockCipher.encryptAesBlockCipher(QJsonDocument(apiPayload).toJson(), encRequestData.key, encRequestData.iv,
                                                                "", encRequestData.salt);
    } catch (...) {
        Utils::logException();
        qCritical() << "error when encrypting the request body";
        encRequestData.errorCode = ErrorCode::ApiConfigDecryptionError;
        return encRequestData;
    }

    QJsonObject requestBody;
    requestBody[apiDefs::key::keyPayload] = QString(encryptedKeyPayload.toBase64());
    requestBody[apiDefs::key::apiPayload] = QString(encryptedApiPayload.toBase64());

    encRequestData.requestBody = QJsonDocument(requestBody).toJson();
    logGatewayPrepared(endpoint, encRequestData.request, encRequestData.requestBody.size());
    return encRequestData;
}

GatewayController::DecryptionResult GatewayController::tryDecryptResponseBody(const QByteArray &encryptedResponseBody,
                                                                              QNetworkReply::NetworkError replyError, const QByteArray &key,
                                                                              const QByteArray &iv, const QByteArray &salt)
{
    DecryptionResult result;
    result.decryptedBody = encryptedResponseBody;
    result.isDecryptionSuccessful = false;

    try {
        QSimpleCrypto::QBlockCipher blockCipher;
        result.decryptedBody = blockCipher.decryptAesBlockCipher(encryptedResponseBody, key, iv, "", salt);
        result.isDecryptionSuccessful = true;
    } catch (...) {
        result.decryptedBody = encryptedResponseBody;
        result.isDecryptionSuccessful = false;
    }

    return result;
}

ErrorCode GatewayController::post(const QString &endpoint, const QJsonObject apiPayload, QByteArray &responseBody)
{
    EncryptedRequestData encRequestData = prepareRequest(endpoint, apiPayload);
    if (encRequestData.errorCode != ErrorCode::NoError) {
        return encRequestData.errorCode;
    }
    logGatewayRequest(endpoint, encRequestData.request, apiPayload, m_requestTimeoutMsecs);

    QByteArray encryptedResponseBody;
    QString replyErrorString;
    auto replyError = QNetworkReply::NoError;
    int httpStatusCode = 0;
    QList<QSslError> sslErrors;
    DecryptionResult decryptionResult;

    auto requestFunction = [&encRequestData](const QString &url) {
        encRequestData.request.setUrl(url);
        return networkManagerForCurrentThread()->post(encRequestData.request, encRequestData.requestBody);
    };

    auto replyProcessingFunction = [&encryptedResponseBody, &replyErrorString, &replyError, &httpStatusCode, &sslErrors, &encRequestData,
                                    &decryptionResult, endpoint, this](QNetworkReply *reply, const QList<QSslError> &nestedSslErrors) {
        encryptedResponseBody = reply->readAll();
        replyErrorString = QString("%1 url=%2").arg(reply->errorString(), reply->url().toString());
        replyError = reply->error();
        httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        decryptionResult =
                tryDecryptResponseBody(encryptedResponseBody, replyError, encRequestData.key, encRequestData.iv, encRequestData.salt);

        sslErrors = nestedSslErrors;
        logGatewayResponse(QStringLiteral("sync"), endpoint, reply->url(), replyError, replyErrorString, httpStatusCode, sslErrors,
                           encryptedResponseBody.size(), decryptionResult.isDecryptionSuccessful, decryptionResult.decryptedBody.size());
        return sslErrors.isEmpty()
               && !shouldBypassProxy(replyError, decryptionResult.decryptedBody, decryptionResult.isDecryptionSuccessful);
    };

    bool proxySucceeded = false;
    const auto serviceType = apiPayload.value(apiDefs::key::serviceType).toString("");
    const auto userCountryCode = apiPayload.value(apiDefs::key::userCountryCode).toString("");
    bypassProxy(endpoint, serviceType, userCountryCode, requestFunction,
                [&proxySucceeded, &replyProcessingFunction](QNetworkReply *reply, const QList<QSslError> &nestedSslErrors) {
                    proxySucceeded = replyProcessingFunction(reply, nestedSslErrors);
                    return proxySucceeded;
                });

    if (!proxySucceeded) {
        const QUrl directUrl = QUrl(endpoint.arg(m_gatewayEndpoint));
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_direct_fallback"
            << "endpoint_template=" + endpoint
            << "url_host=" + directUrl.host();
        QNetworkReply *reply = requestFunction(endpoint.arg(m_gatewayEndpoint));

        QEventLoop wait;
        connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        connect(reply, &QNetworkReply::sslErrors, [this, &sslErrors](const QList<QSslError> &errors) { sslErrors = errors; });
        wait.exec(QEventLoop::ExcludeUserInputEvents);

        replyProcessingFunction(reply, sslErrors);
        reply->deleteLater();
    }

    auto errorCode =
            apiUtils::checkNetworkReplyErrors(sslErrors, replyErrorString, replyError, httpStatusCode, decryptionResult.decryptedBody);
    if (errorCode) {
        return errorCode;
    }

    if (!decryptionResult.isDecryptionSuccessful) {
        qWarning().noquote()
            << "AmneziaDiagnostic event=api_decrypt_failure"
            << "endpoint_template=" + endpoint
            << "reply_error=" + QString::number(static_cast<int>(replyError))
            << "http_status=" + QString::number(httpStatusCode)
            << "encrypted_bytes=" + QString::number(encryptedResponseBody.size());
        qCritical() << "error when decrypting the request body";
        return ErrorCode::ApiConfigDecryptionError;
    }

    responseBody = decryptionResult.decryptedBody;
    return ErrorCode::NoError;
}

QFuture<QPair<ErrorCode, QByteArray>> GatewayController::postAsync(const QString &endpoint, const QJsonObject apiPayload)
{
    auto promise = QSharedPointer<QPromise<QPair<ErrorCode, QByteArray>>>::create();
    promise->start();

    EncryptedRequestData encRequestData = prepareRequest(endpoint, apiPayload);
    if (encRequestData.errorCode != ErrorCode::NoError) {
        promise->addResult(qMakePair(encRequestData.errorCode, QByteArray()));
        promise->finish();
        return promise->future();
    }
    logGatewayRequest(endpoint, encRequestData.request, apiPayload, m_requestTimeoutMsecs);

    auto processResponse = [promise](const GatewayController::DecryptionResult &decryptionResult,
                                     const QList<QSslError> &sslErrors, QNetworkReply::NetworkError replyError,
                                     const QString &replyErrorString, int httpStatusCode) {
        auto errorCode = apiUtils::checkNetworkReplyErrors(sslErrors, replyErrorString, replyError, httpStatusCode,
                                                           decryptionResult.decryptedBody);
        if (errorCode) {
            promise->addResult(qMakePair(errorCode, QByteArray()));
            promise->finish();
            return;
        }

        if (!decryptionResult.isDecryptionSuccessful) {
            qWarning().noquote()
                << "AmneziaDiagnostic event=api_decrypt_failure"
                << "reply_error=" + QString::number(static_cast<int>(replyError))
                << "http_status=" + QString::number(httpStatusCode)
                << "decrypted_bytes=" + QString::number(decryptionResult.decryptedBody.size());
            Utils::logException();
            qCritical() << "error when decrypting the request body";
            promise->addResult(qMakePair(ErrorCode::ApiConfigDecryptionError, QByteArray()));
            promise->finish();
            return;
        }

        promise->addResult(qMakePair(ErrorCode::NoError, decryptionResult.decryptedBody));
        promise->finish();
    };

    auto sendDirect = [this, endpoint, encRequestData, processResponse]() mutable {
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_direct_fallback"
            << "endpoint_template=" + endpoint
            << "url_host=" + encRequestData.request.url().host();
        QNetworkReply *reply = networkManagerForCurrentThread()->post(encRequestData.request, encRequestData.requestBody);
        auto sslErrors = QSharedPointer<QList<QSslError>>::create();
        connect(reply, &QNetworkReply::sslErrors, [sslErrors](const QList<QSslError> &errors) { *sslErrors = errors; });
        connect(reply, &QNetworkReply::finished, reply, [this, reply, sslErrors, encRequestData, endpoint, processResponse]() mutable {
            QByteArray encryptedResponseBody = reply->readAll();
            QString replyErrorString = QString("%1 url=%2").arg(reply->errorString(), reply->url().toString());
            QUrl replyUrl = reply->url();
            auto replyError = reply->error();
            int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();

            auto decryptionResult =
                    tryDecryptResponseBody(encryptedResponseBody, replyError, encRequestData.key, encRequestData.iv, encRequestData.salt);
            logGatewayResponse(QStringLiteral("async-direct"), endpoint, replyUrl, replyError, replyErrorString, httpStatusCode,
                               *sslErrors, encryptedResponseBody.size(), decryptionResult.isDecryptionSuccessful,
                               decryptionResult.decryptedBody.size());
            processResponse(decryptionResult, *sslErrors, replyError, replyErrorString, httpStatusCode);
        });
    };

    const auto serviceType = apiPayload.value(apiDefs::key::serviceType).toString("");
    const auto userCountryCode = apiPayload.value(apiDefs::key::userCountryCode).toString("");
    const QStringList proxyStorageUrls = buildProxyStorageUrls(m_isDevEnvironment, serviceType, userCountryCode);
    getProxyUrlsAsync(proxyStorageUrls, 0, [this, encRequestData, endpoint, processResponse, sendDirect](const QStringList &proxyUrls) mutable {
        getProxyUrlAsync(proxyUrls, 0, [this, encRequestData, endpoint, processResponse, sendDirect](const QString &proxyUrl) mutable {
            if (proxyUrl.isEmpty()) {
                sendDirect();
                return;
            }
            bypassProxyAsync(endpoint, proxyUrl, encRequestData,
                             [this, processResponse, sendDirect](const QByteArray &decryptedBody, bool isDecryptionSuccessful,
                                                                 const QList<QSslError> &sslErrors, QNetworkReply::NetworkError replyError,
                                                                 const QString &replyErrorString, int httpStatusCode) mutable {
                                 GatewayController::DecryptionResult result;
                                 result.decryptedBody = decryptedBody;
                                 result.isDecryptionSuccessful = isDecryptionSuccessful;
                                 if (sslErrors.isEmpty()
                                     && shouldBypassProxy(replyError, result.decryptedBody, result.isDecryptionSuccessful)) {
                                     sendDirect();
                                     return;
                                 }
                                 processResponse(result, sslErrors, replyError, replyErrorString, httpStatusCode);
                             });
        });
    });

    return promise->future();
}

QStringList GatewayController::getProxyUrls(const QString &serviceType, const QString &userCountryCode)
{
    QNetworkRequest request;
    request.setTransferTimeout(proxyStorageRequestTimeoutMsecs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QEventLoop wait;
    QList<QSslError> sslErrors;
    QNetworkReply *reply;

    QByteArray key = agwPublicKey(m_isDevEnvironment);
    QStringList proxyStorageUrls = buildProxyStorageUrls(m_isDevEnvironment, serviceType, userCountryCode);

    if (proxyStorageUrls.empty()) {
        qDebug() << "empty storage endpoint list";
        return {};
    }

    for (const auto &proxyStorageUrl : proxyStorageUrls) {
        request.setUrl(proxyStorageUrl);
        reply = networkManagerForCurrentThread()->get(request);

        connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        connect(reply, &QNetworkReply::sslErrors, [this, &sslErrors](const QList<QSslError> &errors) { sslErrors = errors; });
        wait.exec(QEventLoop::ExcludeUserInputEvents);

        if (reply->error() == QNetworkReply::NetworkError::NoError) {
            auto encryptedResponseBody = reply->readAll();
            reply->deleteLater();

            EVP_PKEY *privateKey = nullptr;
            QByteArray responseBody;
            try {
                if (!m_isDevEnvironment) {
                    QCryptographicHash hash(QCryptographicHash::Sha512);
                    hash.addData(key);
                    QByteArray hashResult = hash.result().toHex();

                    QByteArray key = QByteArray::fromHex(hashResult.left(64));
                    QByteArray iv = QByteArray::fromHex(hashResult.mid(64, 32));

                    QByteArray ba = QByteArray::fromBase64(encryptedResponseBody);

                    QSimpleCrypto::QBlockCipher blockCipher;
                    responseBody = blockCipher.decryptAesBlockCipher(ba, key, iv);
                } else {
                    responseBody = encryptedResponseBody;
                }
            } catch (...) {
                Utils::logException();
                qCritical() << "error loading private key from environment variables or decrypting payload" << encryptedResponseBody;
                continue;
            }

            auto endpointsArray = QJsonDocument::fromJson(responseBody).array();

            QStringList endpoints;
            for (const auto &endpoint : endpointsArray) {
                endpoints.push_back(endpoint.toString());
            }
            return endpoints;
        } else {
            auto replyError = reply->error();
            int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qDebug() << replyError;
            qDebug() << httpStatusCode;
            qDebug() << "go to the next storage endpoint";

            reply->deleteLater();
        }
    }
    return {};
}

bool GatewayController::shouldBypassProxy(const QNetworkReply::NetworkError &replyError, const QByteArray &decryptedResponseBody,
                                          bool isDecryptionSuccessful)
{
    const QByteArray &responseBody = decryptedResponseBody;

    int apiHttpStatus = -1;
    QString apiErrorMessage;
    if (replyError == QNetworkReply::NetworkError::OperationCanceledError || replyError == QNetworkReply::NetworkError::TimeoutError) {
        qDebug() << "timeout occurred";
        qDebug() << replyError;
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_decision"
            << "decision=try_next_proxy"
            << "reason=timeout"
            << "reply_error=" + QString::number(static_cast<int>(replyError));
        return true;
    }

    if (isDecryptionSuccessful) {
        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseBody);
        if (jsonDoc.isObject()) {
            QJsonObject jsonObj = jsonDoc.object();
            apiHttpStatus = jsonObj.value("http_status").toInt(-1);
            apiErrorMessage = jsonObj.value(QStringLiteral("message")).toString().trimmed();
        }
    } else {
        qDebug() << "failed to decrypt the data";
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_decision"
            << "decision=try_next_proxy"
            << "reason=decrypt_failed"
            << "reply_error=" + QString::number(static_cast<int>(replyError))
            << "response_bytes=" + QString::number(responseBody.size());
        return true;
    }

    if (responseBody.contains("html")) {
        qDebug() << "the response contains an html tag";
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_decision"
            << "decision=try_next_proxy"
            << "reason=html_response";
        return true;
    } 
    if (apiHttpStatus == httpStatusCodeNotFound) {
        if (responseBody.contains(errorResponsePattern1) || responseBody.contains(errorResponsePattern2)
            || responseBody.contains(errorResponsePattern3)) {
            return false;
        } else {
            qDebug() << replyError;
            return true;
        }
    } 
    if (apiHttpStatus == httpStatusCodeNotImplemented) {
        if (responseBody.contains(updateRequestResponsePattern)) {
            return false;
        } else {
            qDebug() << replyError;
            return true;
        }
    } 
    if (apiHttpStatus == httpStatusCodeConflict) {
        return false;
    } 
    if (apiHttpStatus == httpStatusCodePaymentRequired) {
        return false;
    } 
    if (apiHttpStatus == httpStatusCodeUnprocessableEntity) {
        return apiErrorMessage != unprocessableSubscriptionMessage;
    } 
    if (replyError != QNetworkReply::NetworkError::NoError) {
        qDebug() << replyError;
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_decision"
            << "decision=try_next_proxy"
            << "reason=network_error"
            << "reply_error=" + QString::number(static_cast<int>(replyError));
        return true;
    }
    qInfo().noquote()
        << "AmneziaDiagnostic event=api_proxy_decision"
        << "decision=accept_response"
        << "reply_error=" + QString::number(static_cast<int>(replyError));
    return false;
}

void GatewayController::bypassProxy(const QString &endpoint, const QString &serviceType, const QString &userCountryCode,
                                    std::function<QNetworkReply *(const QString &url)> requestFunction,
                                    std::function<bool(QNetworkReply *reply, const QList<QSslError> &sslErrors)> replyProcessingFunction)
{
    QStringList proxyUrls = getProxyUrls(serviceType, userCountryCode);
    qInfo().noquote()
        << "AmneziaDiagnostic event=api_proxy_list"
        << "endpoint_template=" + endpoint
        << "proxy_count=" + QString::number(proxyUrls.size())
        << "service_type=" + serviceType
        << "user_country=" + userCountryCode;
    std::random_device randomDevice;
    std::mt19937 generator(randomDevice());
    std::shuffle(proxyUrls.begin(), proxyUrls.end(), generator);

    QByteArray responseBody;

    auto bypassFunction = [this](const QString &endpoint, const QString &proxyUrl,
                                 std::function<QNetworkReply *(const QString &url)> requestFunction,
                                 std::function<bool(QNetworkReply * reply, const QList<QSslError> &sslErrors)> replyProcessingFunction) {
        QEventLoop wait;
        QList<QSslError> sslErrors;

        qDebug() << "go to the next proxy endpoint";
        QNetworkReply *reply = requestFunction(endpoint.arg(proxyUrl));
        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_attempt"
            << "endpoint_template=" + endpoint
            << "url_host=" + reply->url().host()
            << "url_path=" + reply->url().path();

        QObject::connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        connect(reply, &QNetworkReply::sslErrors, [this, &sslErrors](const QList<QSslError> &errors) { sslErrors = errors; });
        wait.exec(QEventLoop::ExcludeUserInputEvents);

        auto result = replyProcessingFunction(reply, sslErrors);
        reply->deleteLater();
        return result;
    };

    if (m_proxyUrl.isEmpty()) {
        QNetworkRequest request;
        request.setTransferTimeout(1000);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QEventLoop wait;
        QList<QSslError> sslErrors;
        QNetworkReply *reply;

        for (const QString &proxyUrl : proxyUrls) {
            request.setUrl(proxyUrl + "lmbd-health");
            reply = networkManagerForCurrentThread()->get(request);
            qInfo().noquote()
                << "AmneziaDiagnostic event=api_proxy_health_attempt"
                << "url_host=" + request.url().host()
                << "url_path=" + request.url().path();

            connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
            connect(reply, &QNetworkReply::sslErrors, [this, &sslErrors](const QList<QSslError> &errors) { sslErrors = errors; });
            wait.exec(QEventLoop::ExcludeUserInputEvents);

            if (reply->error() == QNetworkReply::NetworkError::NoError) {
                qInfo().noquote()
                    << "AmneziaDiagnostic event=api_proxy_health_result"
                    << "url_host=" + reply->url().host()
                    << "reply_error=0";
                reply->deleteLater();

                m_proxyUrl = proxyUrl;
                if (!m_proxyUrl.isEmpty()) {
                    break;
                }
            } else {
                qInfo().noquote()
                    << "AmneziaDiagnostic event=api_proxy_health_result"
                    << "url_host=" + reply->url().host()
                    << "reply_error=" + QString::number(static_cast<int>(reply->error()))
                    << "reply_error_text=" + reply->errorString();
                reply->deleteLater();
            }
        }
    }

    if (!m_proxyUrl.isEmpty()) {
        if (bypassFunction(endpoint, m_proxyUrl, requestFunction, replyProcessingFunction)) {
            return;
        }
    }

    for (const QString &proxyUrl : proxyUrls) {
        if (bypassFunction(endpoint, proxyUrl, requestFunction, replyProcessingFunction)) {
            m_proxyUrl = proxyUrl;
            break;
        }
    }
}

void GatewayController::getProxyUrlsAsync(const QStringList proxyStorageUrls, const int /*unusedStartIndex*/,
                                          std::function<void(const QStringList &)> onComplete)
{
    // Race all storage URLs in parallel instead of sequentially-with-3s-timeout-each.
    // Old behaviour gave up to ~24s of dead wait when every storage CDN was slow,
    // blocking the user from doing anything else (e.g. switching region). With
    // parallel races the worst case is one timeout, not the sum.
    // First reply that returns a non-empty decrypted endpoint list wins; the
    // rest are aborted. Per-reply decrypt/empty failures don't end the race.
    if (proxyStorageUrls.isEmpty()) {
        onComplete({});
        return;
    }

    struct State {
        QList<QPointer<QNetworkReply>> replies;
        int finishedCount = 0;
        bool decided = false;
    };
    auto state = QSharedPointer<State>::create();
    auto onCompleteShared = QSharedPointer<std::function<void(const QStringList &)>>::create(std::move(onComplete));

    auto finishOnce = [state, onCompleteShared](const QStringList &result, QNetworkReply *winner) {
        if (state->decided) return;
        state->decided = true;
        for (const QPointer<QNetworkReply> &r : state->replies) {
            if (r && r.data() != winner) {
                r->abort();
                r->deleteLater();
            }
        }
        (*onCompleteShared)(result);
    };

    const int total = proxyStorageUrls.size();
    for (const QString &storageUrl : proxyStorageUrls) {
        QNetworkRequest request;
        request.setTransferTimeout(proxyStorageRequestTimeoutMsecs);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setUrl(storageUrl);

        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_storage_attempt"
            << "url_host=" + request.url().host()
            << "url_path=" + request.url().path();
        QNetworkReply *reply = networkManagerForCurrentThread()->get(request);
        state->replies.append(reply);

        connect(reply, &QNetworkReply::finished, this, [this, reply, state, total, finishOnce]() {
            if (state->decided) {
                reply->deleteLater();
                return;
            }
            state->finishedCount++;

            const bool replyOk = (reply->error() == QNetworkReply::NoError);
            qInfo().noquote()
                << "AmneziaDiagnostic event=api_proxy_storage_result"
                << "url_host=" + reply->url().host()
                << "reply_error=" + QString::number(static_cast<int>(reply->error()))
                << "reply_error_text=" + reply->errorString();
            QByteArray encrypted = replyOk ? reply->readAll() : QByteArray();
            reply->deleteLater();

            if (replyOk && !encrypted.isEmpty()) {
                try {
                    QByteArray key = agwPublicKey(m_isDevEnvironment);
                    QByteArray responseBody;
                    if (!m_isDevEnvironment) {
                        QCryptographicHash hash(QCryptographicHash::Sha512);
                        hash.addData(key);
                        QByteArray h = hash.result().toHex();
                        QByteArray decKey = QByteArray::fromHex(h.left(64));
                        QByteArray iv = QByteArray::fromHex(h.mid(64, 32));
                        QByteArray ba = QByteArray::fromBase64(encrypted);
                        QSimpleCrypto::QBlockCipher cipher;
                        responseBody = cipher.decryptAesBlockCipher(ba, decKey, iv);
                    } else {
                        responseBody = encrypted;
                    }
                    QJsonArray endpointsArray = QJsonDocument::fromJson(responseBody).array();
                    QStringList endpoints;
                    for (const QJsonValue &endpoint : endpointsArray) {
                        endpoints.push_back(endpoint.toString());
                    }
                    if (!endpoints.isEmpty()) {
                        std::random_device randomDevice;
                        std::mt19937 generator(randomDevice());
                        std::shuffle(endpoints.begin(), endpoints.end(), generator);
                        finishOnce(endpoints, nullptr);
                        return;
                    }
                } catch (...) {
                    Utils::logException();
                    qCritical() << "getProxyUrlsAsync: one storage URL decrypt failed — racing others";
                }
            } else if (!replyOk) {
                qDebug() << "getProxyUrlsAsync: storage URL failed err=" << reply->error();
            }

            if (state->finishedCount >= total) {
                finishOnce({}, nullptr);
            }
        });
    }
}

void GatewayController::getProxyUrlAsync(const QStringList proxyUrls, const int /*unusedStartIndex*/,
                                         std::function<void(const QString &)> onComplete)
{
    // Same race-in-parallel rationale as getProxyUrlsAsync: probe every proxy's
    // lmbd-health endpoint concurrently, first NoError wins, rest aborted.
    // 1s timeout per probe × N proxies sequentially used to be Ns of waste;
    // now it's ≤1s.
    if (proxyUrls.isEmpty()) {
        onComplete(QString());
        return;
    }

    struct State {
        QList<QPointer<QNetworkReply>> replies;
        int finishedCount = 0;
        bool decided = false;
    };
    auto state = QSharedPointer<State>::create();
    auto onCompleteShared = QSharedPointer<std::function<void(const QString &)>>::create(std::move(onComplete));

    auto finishOnce = [state, onCompleteShared](const QString &winner, QNetworkReply *winnerReply) {
        if (state->decided) return;
        state->decided = true;
        for (const QPointer<QNetworkReply> &r : state->replies) {
            if (r && r.data() != winnerReply) {
                r->abort();
                r->deleteLater();
            }
        }
        (*onCompleteShared)(winner);
    };

    const int total = proxyUrls.size();
    for (const QString &proxyUrl : proxyUrls) {
        QNetworkRequest request;
        request.setTransferTimeout(1000);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setUrl(proxyUrl + "lmbd-health");

        qInfo().noquote()
            << "AmneziaDiagnostic event=api_proxy_health_attempt"
            << "url_host=" + request.url().host()
            << "url_path=" + request.url().path();
        QNetworkReply *reply = networkManagerForCurrentThread()->get(request);
        state->replies.append(reply);

        connect(reply, &QNetworkReply::finished, this, [this, reply, state, total, proxyUrl, finishOnce]() {
            if (state->decided) {
                reply->deleteLater();
                return;
            }
            state->finishedCount++;
            const bool replyOk = (reply->error() == QNetworkReply::NoError);
            qInfo().noquote()
                << "AmneziaDiagnostic event=api_proxy_health_result"
                << "url_host=" + reply->url().host()
                << "reply_error=" + QString::number(static_cast<int>(reply->error()))
                << "reply_error_text=" + reply->errorString();
            reply->deleteLater();

            if (replyOk) {
                m_proxyUrl = proxyUrl;
                finishOnce(proxyUrl, nullptr);
                return;
            }
            if (state->finishedCount >= total) {
                finishOnce(QString(), nullptr);
            }
        });
    }
}

void GatewayController::bypassProxyAsync(
        const QString &endpoint, const QString &proxyUrl, EncryptedRequestData encRequestData,
        std::function<void(const QByteArray &, bool, const QList<QSslError> &, QNetworkReply::NetworkError, const QString &, int)> onComplete)
{
    auto sslErrors = QSharedPointer<QList<QSslError>>::create();
    if (proxyUrl.isEmpty()) {
        onComplete(QByteArray(), false, *sslErrors, QNetworkReply::InternalServerError, "empty proxy url", 0);
        return;
    }

    QNetworkRequest request = encRequestData.request;
    request.setUrl(endpoint.arg(proxyUrl));

    qInfo().noquote()
        << "AmneziaDiagnostic event=api_proxy_attempt"
        << "endpoint_template=" + endpoint
        << "url_host=" + request.url().host()
        << "url_path=" + request.url().path();
    QNetworkReply *reply = networkManagerForCurrentThread()->post(request, encRequestData.requestBody);

    connect(reply, &QNetworkReply::sslErrors, this, [sslErrors](const QList<QSslError> &errors) { *sslErrors = errors; });

    connect(reply, &QNetworkReply::finished, this, [sslErrors, onComplete, encRequestData, endpoint, reply, this]() {
        QByteArray encryptedResponseBody = reply->readAll();
        QString replyErrorString = QString("%1 url=%2").arg(reply->errorString(), reply->url().toString());
        auto replyError = reply->error();
        int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        reply->deleteLater();

        auto decryptionResult =
                tryDecryptResponseBody(encryptedResponseBody, replyError, encRequestData.key, encRequestData.iv, encRequestData.salt);
        logGatewayResponse(QStringLiteral("async-proxy"), endpoint, reply->url(), replyError, replyErrorString, httpStatusCode,
                           *sslErrors, encryptedResponseBody.size(), decryptionResult.isDecryptionSuccessful,
                           decryptionResult.decryptedBody.size());

        onComplete(decryptionResult.decryptedBody, decryptionResult.isDecryptionSuccessful, *sslErrors, replyError, replyErrorString,
                   httpStatusCode);
    });
}
