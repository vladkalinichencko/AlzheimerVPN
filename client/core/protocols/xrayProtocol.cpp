#include "xrayProtocol.h"

#include "core/protocols/protocolUtils.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/ipcClient.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/serialization/serialization.h"
#include "core/utils/splitTunnelRule.h"
#include "ipc.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QtCore/qlogging.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qprocess.h>

#include <exception>

#ifndef AMNEZIA_XRAY_TUN_NAME
#define AMNEZIA_XRAY_TUN_NAME "utun22"
#endif

#ifdef Q_OS_MACOS
static const QString tunName = QString::fromLatin1(AMNEZIA_XRAY_TUN_NAME);
#else
static const QString tunName = "tun2";
#endif

namespace {
QString xrayDomainMatcher(const amnezia::SplitTunnelRule &rule)
{
    switch (rule.type()) {
    case amnezia::SplitTunnelRule::Type::ExactHost:
        return QStringLiteral("full:") + rule.normalizedText();
    case amnezia::SplitTunnelRule::Type::WildcardHost: {
        const QString pattern = rule.normalizedText();
        if (pattern.startsWith(QStringLiteral("*."))) {
            return QStringLiteral("domain:") + pattern.mid(2);
        }
        QString regex = QRegularExpression::escape(pattern);
        regex.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
        return QStringLiteral("regexp:^") + regex + QStringLiteral("$");
    }
    case amnezia::SplitTunnelRule::Type::IpSubnet:
        break;
    }
    return {};
}

void applyXraySplitTunnelDirectRouting(QJsonObject &xrayConfig, const QJsonObject &rawConfig)
{
    if (static_cast<amnezia::RouteMode>(rawConfig.value(amnezia::configKey::splitTunnelType).toInt())
        != amnezia::RouteMode::VpnAllExceptSites) {
        return;
    }

    QJsonArray domains;
    QSet<QString> seenDomains;
    for (const QJsonValue &value : rawConfig.value(amnezia::configKey::splitTunnelDnsRules).toArray()) {
        const amnezia::SplitTunnelRule rule = amnezia::SplitTunnelRule::fromText(value.toString());
        if (!rule.isValid() || rule.type() == amnezia::SplitTunnelRule::Type::IpSubnet) {
            continue;
        }

        const QString matcher = xrayDomainMatcher(rule);
        if (!matcher.isEmpty() && !seenDomains.contains(matcher)) {
            seenDomains.insert(matcher);
            domains.append(matcher);
        }
    }
    if (domains.isEmpty()) {
        return;
    }

    QJsonArray inbounds = xrayConfig.value(QStringLiteral("inbounds")).toArray();
    for (int i = 0; i < inbounds.size(); ++i) {
        QJsonObject inbound = inbounds.at(i).toObject();
        inbound.insert(QStringLiteral("sniffing"), QJsonObject{
            {QStringLiteral("enabled"), true},
            {QStringLiteral("destOverride"), QJsonArray{
                QStringLiteral("http"),
                QStringLiteral("tls"),
                QStringLiteral("quic")
            }},
            {QStringLiteral("routeOnly"), true}
        });
        inbounds[i] = inbound;
    }
    xrayConfig.insert(QStringLiteral("inbounds"), inbounds);

    QJsonArray outbounds = xrayConfig.value(QStringLiteral("outbounds")).toArray();
    bool hasDirectOutbound = false;
    for (const QJsonValue &value : outbounds) {
        if (value.toObject().value(QStringLiteral("tag")).toString() == QStringLiteral("direct")) {
            hasDirectOutbound = true;
            break;
        }
    }
    if (!hasDirectOutbound) {
        outbounds.append(QJsonObject{
            {QStringLiteral("tag"), QStringLiteral("direct")},
            {QStringLiteral("protocol"), QStringLiteral("freedom")}
        });
        xrayConfig.insert(QStringLiteral("outbounds"), outbounds);
    }

    QJsonObject routing = xrayConfig.value(QStringLiteral("routing")).toObject();
    QJsonArray rules = routing.value(QStringLiteral("rules")).toArray();
    rules.prepend(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("field")},
        {QStringLiteral("domain"), domains},
        {QStringLiteral("outboundTag"), QStringLiteral("direct")}
    });
    routing.insert(QStringLiteral("domainStrategy"), QStringLiteral("AsIs"));
    routing.insert(QStringLiteral("rules"), rules);
    xrayConfig.insert(QStringLiteral("routing"), routing);

    qInfo() << "XrayProtocol::start: split tunnel direct domain routing enabled domains="
            << domains.size();
}
}

XrayProtocol::XrayProtocol(const QJsonObject &configuration, QObject *parent) : VpnProtocol(configuration, parent)
{
    m_vpnGateway = amnezia::protocols::xray::defaultLocalAddr;
    m_vpnLocalAddress = amnezia::protocols::xray::defaultLocalAddr;
    m_routeGateway = NetworkUtilities::getGatewayAndIface().first;

    m_routeMode = static_cast<amnezia::RouteMode>(configuration.value(amnezia::configKey::splitTunnelType).toInt());
    m_remoteAddress = NetworkUtilities::getIPAddress(m_rawConfig.value(amnezia::configKey::hostName).toString());

    const QString primaryDns = configuration.value(amnezia::configKey::dns1).toString();
    m_dnsServers.push_back(QHostAddress(primaryDns));
    if (primaryDns != amnezia::protocols::dns::amneziaDnsIp) {
        const QString secondaryDns = configuration.value(amnezia::configKey::dns2).toString();
        m_dnsServers.push_back(QHostAddress(secondaryDns));
    }

    QJsonObject xrayConfiguration = configuration.value(ProtocolUtils::key_proto_config_data(Proto::Xray)).toObject();
    if (xrayConfiguration.isEmpty()) {
        xrayConfiguration = configuration.value(ProtocolUtils::key_proto_config_data(Proto::SSXray)).toObject();
    }

    if (xrayConfiguration.isEmpty()) {
        qWarning() << "Xray config wrapper is empty";
        m_xrayConfig = {};
    }

    m_xrayConfig = QJsonDocument::fromJson(xrayConfiguration.value(amnezia::configKey::config).toString().toUtf8()).object();
    if (m_xrayConfig.isEmpty()) {
        qWarning() << "Xray config string is not a valid JSON object";
        m_xrayConfig = {};
    }
}

XrayProtocol::~XrayProtocol()
{
    qDebug() << "XrayProtocol::~XrayProtocol()";
    XrayProtocol::stop();
}

ErrorCode XrayProtocol::start()
{
    qDebug() << "XrayProtocol::start()";
    m_stopRequested = false;

    // Inject SOCKS5 auth into the inbound before starting xray.
    // Re-uses existing credentials if the config already has them (e.g. imported config).
    amnezia::serialization::inbounds::InboundCredentials creds;
    try {
        creds = amnezia::serialization::inbounds::EnsureInboundAuth(m_xrayConfig);
    } catch (const std::exception &e) {
        qCritical() << "EnsureInboundAuth failed:" << e.what();
        return ErrorCode::InternalError;
    }
    m_socksUser = creds.username;
    m_socksPassword = creds.password;
    m_socksPort = creds.port;
    applyXraySplitTunnelDirectRouting(m_xrayConfig, m_rawConfig);

    const QString xrayConfigStr = QJsonDocument(m_xrayConfig).toJson(QJsonDocument::Compact);
    if (xrayConfigStr.isEmpty()) {
        qCritical() << "Xray config is empty";
        return ErrorCode::XrayExecutableCrashed;
    }

    return IpcClient::withInterface(
            [&](QSharedPointer<IpcInterfaceReplica> iface) {
                // Note: the kill-switch hole for the VPN server IP is opened in
                // VpnConnection::connectToVpn, before any protocol-specific start.
                // This is the single business-logic entry for all protocols.
                qInfo() << "XrayProtocol::start: invoking iface->xrayStart, config size="
                        << xrayConfigStr.size();
                auto xrayStart = iface->xrayStart(xrayConfigStr);
                const bool finished = xrayStart.waitForFinished();
                const bool returned = finished ? xrayStart.returnValue() : false;
                qInfo() << "XrayProtocol::start: xrayStart finished=" << finished
                        << "returnValue=" << returned;
                if (!finished || !returned) {
                    qCritical() << "XrayProtocol::start: Failed to start xray (finished="
                                << finished << ", returnValue=" << returned << ")";
                    return ErrorCode::XrayExecutableCrashed;
                }
                if (m_stopRequested) {
                    qInfo() << "XrayProtocol::start: stop requested while xrayStart was pending";
                    return ErrorCode::NoError;
                }
                const ErrorCode tunResult = startTun2Socks();
                qInfo() << "XrayProtocol::start: startTun2Socks returned" << static_cast<int>(tunResult);
                return tunResult;
            },
            []() {
                qCritical() << "XrayProtocol::start: withInterface fell to onFailure — IPC unavailable";
                return ErrorCode::AmneziaServiceConnectionFailed;
            });
}

void XrayProtocol::stop()
{
    qDebug() << "XrayProtocol::stop()";
    m_stopRequested = true;

    // Idempotent: re-entering stop() (e.g. from a queued tun2socks finished
    // signal arriving while we're tearing down) used to recurse via
    // waitForFinished spinning a nested event loop. Combined with the chained
    // synchronous IPC calls below, that produced the 25 s hang on close
    // captured in stack-shot 2026-05-20-18:17:50.
    if (m_stopping) {
        return;
    }
    m_stopping = true;

    QSharedPointer<IpcInterfaceReplica> iface = IpcClient::Interface();
    if (!iface.isNull() && iface->isReplicaValid()) {
        // Fire-and-forget. Each prior waitForFinished could spin the event loop
        // and dispatch a queued tun2socks::finished → stop() → recursion → hang.
        IpcClient::async(this, iface->disableKillSwitch(),
            [](bool ok) { if (!ok) qWarning() << "Failed to disable killswitch"; });
        IpcClient::async(this, iface->StartRoutingIpv6(),
            [](bool ok) { if (!ok) qWarning() << "Failed to start routing ipv6"; });
        IpcClient::async(this, iface->restoreResolvers(),
            [](bool ok) { if (!ok) qWarning() << "Failed to restore resolvers"; });
        // Intentionally DO NOT call deleteTun here. It pkills any tun2socks
        // bound to tunName on the daemon side; because we're async, this
        // request can arrive AFTER the user's next Connect has already spawned
        // a fresh tun2socks on the same utun — and the late-arriving pkill
        // kills the fresh one. That produced the "Disconnect → Connect → 803
        // immediately" loop. Cleanup of stale tun2socks is handled defensively
        // at the start of startTun2Socks() (synchronous, with a 1s waitForFinished),
        // which serializes correctly with the new process spawn.
        IpcClient::async(this, iface->xrayStop(),
            [](bool ok) { if (!ok) qWarning() << "Failed to stop xray"; });
    }

    if (m_tun2socksProcess) {
        // Disconnect FIRST so any in-flight queued metacall from previous
        // process state changes cannot re-enter stop().
        QObject::disconnect(m_tun2socksProcess.data(), nullptr, this, nullptr);
        m_tun2socksProcess->blockSignals(true);

#ifndef Q_OS_WIN
        m_tun2socksProcess->terminate();
        // Best-effort kill — do not block on it. The daemon-side deleteTun above
        // also pkills any tun2socks bound to our utun.
        m_tun2socksProcess->kill();
#else
        m_tun2socksProcess->kill();
#endif

        m_tun2socksProcess->close();
        m_tun2socksProcess.reset();
    }

    setConnectionState(Vpn::ConnectionState::Disconnected);
    m_stopping = false;
}

ErrorCode XrayProtocol::startTun2Socks()
{
    qInfo() << "XrayProtocol::startTun2Socks: entry, tunName=" << tunName
            << "socksPort=" << m_socksPort;

    // Defensive cleanup: if a previous tun2socks is still alive on this utun
    // (GUI crashed/restarted without teardown), spawning a new one immediately
    // fails with "create tun: resource busy" → ErrorCode 804. Ask the daemon
    // to free the device (deleteTun also pkills the lingering tun2socks).
    const bool cleanupOk = IpcClient::withInterface([](QSharedPointer<IpcInterfaceReplica> iface) {
        auto deleteTun = iface->deleteTun(tunName);
        const bool ok = deleteTun.waitForFinished(1000);
        const bool freed = ok && deleteTun.returnValue();
        qInfo() << "XrayProtocol::startTun2Socks: defensive deleteTun finished="
                << ok << "returnValue=" << freed;
        return freed;
    }, []() {
        return false;
    });
    if (!cleanupOk) {
        qCritical() << "XrayProtocol::startTun2Socks: TUN cleanup failed before start";
        return ErrorCode::AmneziaServiceConnectionFailed;
    }

    m_tun2socksProcess = IpcClient::CreatePrivilegedProcess();
    // CreatePrivilegedProcess returns a null QSharedPointer when the daemon-side
    // createPrivilegedProcess() slot times out (default QRO timeout, ~30s).
    // Dereferencing here used to SIGSEGV the GUI (crash 2026-05-21-034118.ips
    // line "Failed to create privileged process" followed by null deref in
    // QRemoteObjectReplica::waitForSource). Surface a clean error instead.
    if (m_tun2socksProcess.isNull()) {
        qCritical() << "XrayProtocol::startTun2Socks: privileged process replica is null"
                       " (daemon failed to allocate or replica acquisition failed).";
        return ErrorCode::AmneziaServiceConnectionFailed;
    }
    if (!m_tun2socksProcess->waitForSource()) {
        qCritical() << "XrayProtocol::startTun2Socks: tun2socks replica waitForSource() failed.";
        m_tun2socksProcess.reset();
        return ErrorCode::AmneziaServiceConnectionFailed;
    }

    const QString proxyUrl = QString("socks5://%1:%2@127.0.0.1:%3").arg(m_socksUser, m_socksPassword, QString::number(m_socksPort));

    m_tun2socksProcess->setProgram(PermittedProcess::Tun2Socks);
    m_tun2socksProcess->setArguments({ "-device", QString("tun://%1").arg(tunName), "-proxy", proxyUrl });

    connect(
            m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::readyReadStandardError, this, 
            [this]() {
                auto readAllStandardError = m_tun2socksProcess->readAllStandardError();
                if (!readAllStandardError.waitForFinished()) {
                    qWarning() << "Failed to read output from tun2socks";
                    return;
                }

                const QString line = readAllStandardError.returnValue();

                if (!line.contains("[TCP]") && !line.contains("[UDP]"))
                    qDebug() << "[tun2socks]:" << line;

                if (line.contains("[STACK] tun://") && line.contains("<-> socks5://")) {
                    disconnect(m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::readyReadStandardOutput, this, nullptr);

                    if (ErrorCode res = setupRouting(); res != ErrorCode::NoError) {
                        stop();
                        setLastError(res);
                    } else {
                        setConnectionState(Vpn::ConnectionState::Connected);
                    }
                }
            },
            Qt::QueuedConnection);

    connect(
            m_tun2socksProcess.data(), &IpcProcessInterfaceReplica::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitStatus == QProcess::ExitStatus::CrashExit) {
                    qCritical() << "Tun2socks process crashed!";
                } else {
                    qCritical() << QString("Tun2socks process was closed with %1 exit code").arg(exitCode);
                }
                stop();
                setLastError(ErrorCode::Tun2SockExecutableCrashed);
            },
            Qt::QueuedConnection);

    m_tun2socksProcess->start();
    return ErrorCode::NoError;
}

ErrorCode XrayProtocol::setupRouting()
{
    return IpcClient::withInterface(
            [this](QSharedPointer<IpcInterfaceReplica> iface) -> ErrorCode {
#ifdef Q_OS_WIN
                const int inetAdapterIndex = NetworkUtilities::AdapterIndexTo(QHostAddress(m_remoteAddress));
#endif
                auto createTun = iface->createTun(tunName, amnezia::protocols::xray::defaultLocalAddr);
                if (!createTun.waitForFinished() || !createTun.returnValue()) {
                    qCritical() << "Failed to assign IP address for TUN";
                    return ErrorCode::InternalError;
                }

                auto updateResolvers = iface->updateResolvers(tunName, m_dnsServers);
                if (!updateResolvers.waitForFinished() || !updateResolvers.returnValue()) {
                    qCritical() << "Failed to set DNS resolvers for TUN";
                    return ErrorCode::InternalError;
                }

#ifdef Q_OS_WIN
                int vpnAdapterIndex = -1;
                QList<QNetworkInterface> netInterfaces = QNetworkInterface::allInterfaces();
                for (auto &netInterface : netInterfaces) {
                    for (auto &address : netInterface.addressEntries()) {
                        if (m_vpnLocalAddress == address.ip().toString())
                            vpnAdapterIndex = netInterface.index();
                    }
                }
#else
                static const int vpnAdapterIndex = 0;
#endif
                const bool killSwitchEnabled = QVariant(m_rawConfig.value(configKey::killSwitchOption).toString()).toBool();
                if (killSwitchEnabled) {
                    if (vpnAdapterIndex != -1) {
                        QJsonObject config = m_rawConfig;
                        config.insert("vpnServer", m_remoteAddress);

                        auto enableKillSwitch = IpcClient::Interface()->enableKillSwitch(config, vpnAdapterIndex);
                        if (!enableKillSwitch.waitForFinished() || !enableKillSwitch.returnValue()) {
                            qCritical() << "Failed to enable killswitch";
                            return ErrorCode::InternalError;
                        }
                    } else
                        qWarning() << "Failed to get vpnAdapterIndex. Killswitch disabled";
                }

                if (m_routeMode == amnezia::RouteMode::VpnAllSites) {
                    static const QStringList subnets = { "1.0.0.0/8",  "2.0.0.0/7",  "4.0.0.0/6",  "8.0.0.0/5",
                                                         "16.0.0.0/4", "32.0.0.0/3", "64.0.0.0/2", "128.0.0.0/1" };

                    auto routeAddList = iface->routeAddList(m_vpnGateway, subnets);
                    if (!routeAddList.waitForFinished() || routeAddList.returnValue() != subnets.count()) {
                        qCritical() << "Failed to set routes for TUN";
                        return ErrorCode::InternalError;
                    }
                }

                auto StopRoutingIpv6 = iface->StopRoutingIpv6();
                if (!StopRoutingIpv6.waitForFinished() || !StopRoutingIpv6.returnValue()) {
                    qCritical() << "Failed to disable IPv6 routing";
                    return ErrorCode::InternalError;
                }

#ifdef Q_OS_WIN
                if (inetAdapterIndex != -1 && vpnAdapterIndex != -1) {
                    QJsonObject config = m_rawConfig;
                    config.insert("inetAdapterIndex", inetAdapterIndex);
                    config.insert("vpnAdapterIndex", vpnAdapterIndex);
                    config.insert("vpnGateway", m_vpnGateway);
                    config.insert("vpnServer", m_remoteAddress);

                    auto enablePeerTraffic = iface->enablePeerTraffic(config);
                    if (!enablePeerTraffic.waitForFinished() || !enablePeerTraffic.returnValue()) {
                        qCritical() << "Failed to enable peer traffic";
                        return ErrorCode::InternalError;
                    }
                } else
                    qWarning() << "Failed to get adapter indexes. Split-tunneling disabled";
#endif
                return ErrorCode::NoError;
            },
            []() { return ErrorCode::AmneziaServiceConnectionFailed; });
}
