#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QtTest>

#include "core/utils/api/requestDeadline.h"

// A server that accepts connections and never answers, like a blackholed gateway.
class RequestDeadlineTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
    }

    void cleanup()
    {
        m_server.close();
    }

    void abortsSilentServerAtDeadline()
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        request.setTransferTimeout(30000);
        QNetworkReply *reply = manager.get(request);

        QElapsedTimer elapsed;
        elapsed.start();
        apiUtils::abortReplyAtDeadline(reply, QDateTime::currentMSecsSinceEpoch() + 300);
        QEventLoop wait;
        connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();

        QCOMPARE(reply->error(), QNetworkReply::OperationCanceledError);
        QVERIFY2(elapsed.elapsed() >= 250, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY2(elapsed.elapsed() < 3000, qPrintable(QString::number(elapsed.elapsed())));
        reply->deleteLater();
    }

    void pastDeadlineAbortsImmediately()
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        QNetworkReply *reply = manager.get(request);

        QElapsedTimer elapsed;
        elapsed.start();
        apiUtils::abortReplyAtDeadline(reply, QDateTime::currentMSecsSinceEpoch() - 1000);
        QEventLoop wait;
        connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();

        QCOMPARE(reply->error(), QNetworkReply::OperationCanceledError);
        QVERIFY(elapsed.elapsed() < 1000);
        reply->deleteLater();
    }

    void zeroDeadlineLeavesReplyAlone()
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        request.setTransferTimeout(600);
        QNetworkReply *reply = manager.get(request);

        apiUtils::abortReplyAtDeadline(reply, 0);
        QEventLoop wait;
        connect(reply, &QNetworkReply::finished, &wait, &QEventLoop::quit);
        wait.exec();

        // Only the transfer timeout ended it, not our helper.
        QCOMPARE(reply->error(), QNetworkReply::TimeoutError);
        reply->deleteLater();
    }

private:
    QTcpServer m_server;
};

QTEST_MAIN(RequestDeadlineTest)
#include "main.moc"
