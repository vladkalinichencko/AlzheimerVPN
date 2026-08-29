#include "vpnConnection.h"

#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <core/configurators/openVpnConfigurator.h>
#include <core/configurators/wireguardConfigurator.h>

#ifdef AMNEZIA_DESKTOP
    #include "core/utils/ipcClient.h"
    #include <core/protocols/wireGuardProtocol.h>
#endif

#ifdef Q_OS_ANDROID
    #include "platforms/android/android_controller.h"
    #include <QThread>

#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    #include "platforms/ios/ios_controller.h"
#endif

#include "core/utils/networkUtilities.h"
#include "core/utils/containers/containerUtils.h"
#include "core/utils/splitTunnelRoutePlanner.h"
#include "core/utils/splitTunnelRule.h"
#include "core/utils/serverConfigUtils.h"
#include "vpnConnection.h"

using namespace ProtocolUtils;

namespace {
constexpr int kMaxConnectedRecoveryAttempts = 3;

ConnectionHealth connectionHealthForError(ErrorCode error)
{
    switch (error) {
    case ErrorCode::AmneziaServiceConnectionFailed:
    case ErrorCode::VpnBackendFailure:
        return ConnectionHealth::LocalServiceUnavailable;
    case ErrorCode::VpnHandshakeTimeout:
        return ConnectionHealth::HandshakeTimeout;
    case ErrorCode::VpnNoTrafficError:
        return ConnectionHealth::NoTraffic;
    default:
        return ConnectionHealth::UnknownFailure;
    }
}

bool recoverableProtocolError(ErrorCode error)
{
    return error == ErrorCode::AmneziaServiceConnectionFailed ||
           error == ErrorCode::VpnBackendFailure ||
           error == ErrorCode::VpnHandshakeTimeout ||
           error == ErrorCode::VpnNoTrafficError;
}

DockerContainer defaultContainerForServer(const SecureServersRepository *repository, const QString &serverId)
{
    if (!repository || serverId.isEmpty()) {
        return DockerContainer::None;
    }

    switch (repository->serverKind(serverId)) {
    case serverConfigUtils::ConfigType::SelfHostedAdmin:
        if (const auto cfg = repository->selfHostedAdminConfig(serverId)) {
            return cfg->defaultContainer;
        }
        break;
    case serverConfigUtils::ConfigType::SelfHostedUser:
        if (const auto cfg = repository->selfHostedUserConfig(serverId)) {
            return cfg->defaultContainer;
        }
        break;
    case serverConfigUtils::ConfigType::Native:
        if (const auto cfg = repository->nativeConfig(serverId)) {
            return cfg->defaultContainer;
        }
        break;
    case serverConfigUtils::ConfigType::AmneziaPremiumV2:
    case serverConfigUtils::ConfigType::AmneziaFreeV3:
    case serverConfigUtils::ConfigType::ExternalPremium:
        if (const auto cfg = repository->apiV2Config(serverId)) {
            return cfg->defaultContainer;
        }
        break;
    case serverConfigUtils::ConfigType::AmneziaPremiumV1:
    case serverConfigUtils::ConfigType::AmneziaFreeV2:
    case serverConfigUtils::ConfigType::Invalid:
    default:
        break;
    }

    return DockerContainer::None;
}
}

VpnConnection::VpnConnection(SecureServersRepository* serversRepository, SecureAppSettingsRepository* appSettingsRepository, QObject *parent)
    : QObject(parent),
      m_serversRepository(serversRepository),
      m_appSettingsRepository(appSettingsRepository),
      m_checkTimer(this),
      m_connectingTimer(this),
      m_healthTimer(this),
      m_connectedRecoveryTimer(this),
      m_pendingNetworkTimer(this)
{
    m_connectingTimer.setSingleShot(true);
    m_connectingTimer.setInterval(30000);
    connect(&m_connectingTimer, &QTimer::timeout, this, &VpnConnection::onConnectingTimedOut);

    m_healthTimer.setInterval(3000);
    connect(&m_healthTimer, &QTimer::timeout, this, &VpnConnection::checkConnectedHealth);

    m_connectedRecoveryTimer.setSingleShot(true);
    m_connectedRecoveryTimer.setInterval(1000);
    connect(&m_connectedRecoveryTimer, &QTimer::timeout, this, [this]() {
        if (m_connectionState != Vpn::ConnectionState::Connected ||
            m_silentReconnectInProgress ||
            m_vpnProtocol.isNull() ||
            m_currentServerId.isEmpty()) {
            return;
        }
        startSilentReconnect();
    });

    m_pendingNetworkTimer.setInterval(1000);
    connect(&m_pendingNetworkTimer, &QTimer::timeout,
            this, &VpnConnection::resumePendingNetworkAction);

    m_dnsFlushDebounce.setSingleShot(true);
    m_dnsFlushDebounce.setInterval(500);
#ifdef AMNEZIA_DESKTOP
    connect(&m_dnsFlushDebounce, &QTimer::timeout, this, []() {
        IpcClient::withInterface([](QSharedPointer<IpcInterfaceReplica> iface) {
            auto reply = iface->flushDns();
            if (!reply.waitForFinished() || !reply.returnValue()) {
                qWarning() << "VpnConnection: debounced flushDns failed";
            }
        });
    });
#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    m_checkTimer.setInterval(1000);
    connect(IosController::Instance(), &IosController::connectionStateChanged, this, &VpnConnection::setConnectionState);
    connect(IosController::Instance(), &IosController::bytesChanged, this, &VpnConnection::onBytesChanged);
#endif
}

VpnConnection::~VpnConnection()
{
}

void VpnConnection::onBytesChanged(quint64 receivedBytes, quint64 sentBytes)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, receivedBytes, sentBytes]() {
            onBytesChanged(receivedBytes, sentBytes);
        }, Qt::QueuedConnection);
        return;
    }

    if (m_connectionState == Vpn::ConnectionState::Connected && receivedBytes > 0) {
        m_healthChecksWithoutTraffic = 0;
    }

    emit bytesChanged(receivedBytes, sentBytes);
}

void VpnConnection::onKillSwitchModeChanged(bool enabled)
{
#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([enabled](QSharedPointer<IpcInterfaceReplica> iface){
        QRemoteObjectPendingReply<bool> reply = iface->refreshKillSwitch(enabled);
        if (reply.waitForFinished() && reply.returnValue())
            qDebug() << "VpnConnection::onKillSwitchModeChanged: Killswitch refreshed";
        else
            qWarning() << "VpnConnection::onKillSwitchModeChanged: Failed to execute remote refreshKillSwitch call";
    });
#endif
}

void VpnConnection::setConnectionDiagnostic(ConnectionHealth health)
{
    setConnectionHealth(health);
}

void VpnConnection::onConnectionStateChanged(Vpn::ConnectionState state)
{
    switch (state) {
    case Vpn::ConnectionState::Connecting:
        setConnectionHealth(m_silentReconnectInProgress
                                ? ConnectionHealth::Recovering
                                : ConnectionHealth::StartingProtocol);
        stopConnectedRecovery();
        startConnectingWatchdog();
        stopConnectivityProbe();
        stopConnectedHealthCheck();
        break;
    case Vpn::ConnectionState::Reconnecting:
        setConnectionHealth(ConnectionHealth::Recovering);
        stopConnectedRecovery();
        startConnectingWatchdog();
        stopConnectivityProbe();
        stopConnectedHealthCheck();
        break;
    case Vpn::ConnectionState::Connected:
        stopConnectingWatchdog();
        setConnectionHealth(ConnectionHealth::ApplyingRoutes);
        break;
    case Vpn::ConnectionState::Disconnected:
        stopConnectingWatchdog();
        stopConnectivityProbe();
        stopConnectedHealthCheck();
        stopConnectedRecovery();
        if (!connectionHealthProblem(m_connectionHealth)) {
            markLastError(ErrorCode::NoError);
        }
        setConnectionHealth(ConnectionHealth::Idle);
        break;
    case Vpn::ConnectionState::Error:
        stopConnectingWatchdog();
        stopConnectivityProbe();
        stopConnectedHealthCheck();
        stopConnectedRecovery();
        if (!connectionHealthProblem(m_connectionHealth)) {
            setConnectionHealth(ConnectionHealth::UnknownFailure);
        }
        break;
    case Vpn::ConnectionState::Preparing:
        setConnectionHealth(ConnectionHealth::PreparingConfig);
        break;
    case Vpn::ConnectionState::Disconnecting:
    case Vpn::ConnectionState::Unknown:
        stopConnectingWatchdog();
        stopConnectivityProbe();
        stopConnectedHealthCheck();
        stopConnectedRecovery();
        break;
    }

#ifdef AMNEZIA_DESKTOP
    if (!m_serversRepository || !m_appSettingsRepository) {
        qCritical() << "VpnConnection::onConnectionStateChanged: repositories not initialized";
        if (state == Vpn::ConnectionState::Connected) {
            startConnectedHealthCheck();
        }
        return;
    }

    const QString defaultServerId = m_serversRepository->defaultServerId();
    DockerContainer container = DockerContainer::None;
    switch (m_serversRepository->serverKind(defaultServerId)) {
    case serverConfigUtils::ConfigType::SelfHostedAdmin: {
        const auto cfg = m_serversRepository->selfHostedAdminConfig(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::SelfHostedUser: {
        const auto cfg = m_serversRepository->selfHostedUserConfig(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::Native: {
        const auto cfg = m_serversRepository->nativeConfig(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::AmneziaPremiumV2:
    case serverConfigUtils::ConfigType::AmneziaFreeV3:
    case serverConfigUtils::ConfigType::ExternalPremium: {
        const auto cfg = m_serversRepository->apiV2Config(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::AmneziaPremiumV1:
    case serverConfigUtils::ConfigType::AmneziaFreeV2:
        break;
    case serverConfigUtils::ConfigType::Invalid:
    default:
        break;
    }

    // AWG / WireGuard route/DNS/split-tunnel management lives entirely in the
    // daemon (Daemon::activate -> WireguardUtilsMacos -> MacosRouteMonitor +
    // DnsUtilsMacos). The legacy IPC path below is OpenVPN-era and would race
    // with the daemon (flushDns->killall mDNSResponder, etc.), so we skip the
    // whole block for WG-family protocols.
    const bool legacyRouting = !ContainerUtils::isAwgContainer(container)
                               && container != DockerContainer::WireGuard;
    if (!legacyRouting) {
#if defined(Q_OS_IOS) || defined(MACOS_NE)
        if (state == Vpn::ConnectionState::Connected ||
            state == Vpn::ConnectionState::Connecting ||
            state == Vpn::ConnectionState::Reconnecting) {
            m_checkTimer.start();
        } else {
            m_checkTimer.stop();
        }
#endif
        if (state == Vpn::ConnectionState::Connected) {
            startConnectedHealthCheck();
        }
        return;
    }

    IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
        switch (state) {
            case Vpn::ConnectionState::Connected: {
                iface->resetIpStack();

                setConnectionHealth(ConnectionHealth::CheckingDns);
                auto flushDns = iface->flushDns();
                if (flushDns.waitForFinished() && flushDns.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully flushed DNS";
                else {
                    setConnectionHealth(ConnectionHealth::DnsFailed);
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to flush DNS";
                }

                if (!ContainerUtils::isAwgContainer(container) && container != DockerContainer::WireGuard) {
                    QString dns1 = m_vpnConfiguration.value(configKey::dns1).toString();
                    QString dns2 = m_vpnConfiguration.value(configKey::dns2).toString();

#ifdef Q_OS_MACOS
                    if (!m_appSettingsRepository->isSitesSplitTunnelingEnabled() || m_appSettingsRepository->routeMode() != amnezia::RouteMode::VpnAllExceptSites) {
                        iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << dns1 << dns2);
                    }
#else
                    iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << dns1 << dns2);
#endif

                    if (m_appSettingsRepository->isSitesSplitTunnelingEnabled()) {
                        iface->routeDeleteList(m_vpnProtocol->vpnGateway(), QStringList() << "0.0.0.0");
                        RouteMode routeMode = m_appSettingsRepository->routeMode();
                        if (routeMode == amnezia::RouteMode::VpnOnlyForwardSites) {
                            QTimer::singleShot(1000, m_vpnProtocol.data(),
                                               [this, routeMode]() {
                                                   configureDnsSplitTunnel(m_vpnProtocol->vpnGateway(), routeMode);
                                                   addSitesRoutes(m_vpnProtocol->vpnGateway(), routeMode);
                                               });
                        } else if (routeMode == amnezia::RouteMode::VpnAllExceptSites) {
                            setConnectionHealth(ConnectionHealth::ApplyingRoutes);
                            auto routeFirstHalf = iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << "0.0.0.0/1");
                            auto routeSecondHalf = iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << "128.0.0.0/1");
                            if (!routeFirstHalf.waitForFinished() || routeFirstHalf.returnValue() != 1 ||
                                !routeSecondHalf.waitForFinished() || routeSecondHalf.returnValue() != 1) {
                                setConnectionHealth(ConnectionHealth::RouteMismatch);
                            }

                            auto remoteRoute = iface->routeAddList(m_vpnProtocol->routeGateway(), QStringList() << remoteAddress());
                            if (!remoteRoute.waitForFinished() || remoteRoute.returnValue() != 1) {
                                setConnectionHealth(ConnectionHealth::RouteMismatch);
                            }
#ifdef Q_OS_MACOS
                            setConnectionHealth(ConnectionHealth::CheckingDns);
                            auto dnsRoutes = iface->routeAddList(m_vpnProtocol->routeGateway(), QStringList() << dns1 << dns2);
                            if (!dnsRoutes.waitForFinished() || dnsRoutes.returnValue() != 2) {
                                setConnectionHealth(ConnectionHealth::DnsFailed);
                            }
#endif
                            configureDnsSplitTunnel(m_vpnProtocol->routeGateway(), routeMode);
                            addSitesRoutes(m_vpnProtocol->routeGateway(), routeMode);
                        }
                    } else {
                        configureDnsSplitTunnel(QString(), amnezia::RouteMode::VpnAllSites);
                    }
                }
            } break;
            case Vpn::ConnectionState::Disconnected:
            case Vpn::ConnectionState::Error: {
                m_dnsFlushDebounce.stop();
                configureDnsSplitTunnel(QString(), amnezia::RouteMode::VpnAllSites);

                IpcClient::async(this, iface->flushDns(), [](bool ok) {
                    if (ok)
                        qDebug() << "VpnConnection::onConnectionStateChanged: Successfully flushed DNS";
                    else
                        qWarning() << "VpnConnection::onConnectionStateChanged: Failed to flush DNS";
                });

                IpcClient::async(this, iface->clearSavedRoutes(), [](bool ok) {
                    if (ok)
                        qDebug() << "VpnConnection::onConnectionStateChanged: Successfully cleared saved routes";
                    else
                        qWarning() << "VpnConnection::onConnectionStateChanged: Failed to clear saved routes";
                });
            } break;
            default:
                break;
        }
    });
#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    if (state == Vpn::ConnectionState::Connected ||
        state == Vpn::ConnectionState::Connecting ||
        state == Vpn::ConnectionState::Reconnecting) {
        m_checkTimer.start();
    } else {
        m_checkTimer.stop();
    }
#endif

    if (state == Vpn::ConnectionState::Connected) {
        startConnectedHealthCheck();
    }
}

const QString &VpnConnection::remoteAddress() const
{
    return m_remoteAddress;
}

void VpnConnection::setRepositories(SecureServersRepository* serversRepository, SecureAppSettingsRepository* appSettingsRepository)
{
    m_serversRepository = serversRepository;
    m_appSettingsRepository = appSettingsRepository;
}

void VpnConnection::addSitesRoutes(const QString &gw, amnezia::RouteMode mode)
{
#ifdef AMNEZIA_DESKTOP
    if (!m_appSettingsRepository || !m_serversRepository) {
        qCritical() << "VpnConnection::addSitesRoutes: repositories not initialized";
        return;
    }

    // Defense in depth: skip the legacy IPC route/flush path for WG-family
    // protocols. The daemon owns DNS and split-tunnel routes for them; running
    // this would race with the daemon and break AmneziaWG handshake retention
    // (see legacyRouting guard in onConnectionStateChanged).
    const DockerContainer container = defaultContainerForServer(m_serversRepository, m_serversRepository->defaultServerId());
    if (ContainerUtils::isAwgContainer(container) ||
        container == DockerContainer::WireGuard) {
        return;
    }

    QStringList ips;
    int hostRules = 0;
    int invalidRules = 0;
    const QVariantMap &m = m_appSettingsRepository->vpnSites(mode);
    for (auto i = m.constBegin(); i != m.constEnd(); ++i) {
        const amnezia::SplitTunnelRule rule = amnezia::SplitTunnelRule::fromText(i.key());
        if (!rule.isValid()) {
            ++invalidRules;
            continue;
        }

        if (rule.type() == amnezia::SplitTunnelRule::Type::IpSubnet) {
            ips.append(rule.normalizedText());
        } else {
            ++hostRules;
            const QStringList siteIps = SecureAppSettingsRepository::siteIpList(i.value());
            for (const QString &ip : siteIps) {
                if (NetworkUtilities::checkIpSubnetFormat(ip)) {
                    ips.append(ip);
                }
            }
        }
    }
    ips.removeDuplicates();

    qInfo() << "VpnConnection::addSitesRoutes mode=" << mode
            << "gateway=" << gw
            << "rules_total=" << m.size()
            << "host_rules=" << hostRules
            << "cached_or_subnet_routes=" << ips.count()
            << "invalid_rules=" << invalidRules;

    addSplitTunnelRoutes(gw, mode, ips);
#endif
}

void VpnConnection::configureDnsSplitTunnel(const QString &gw, amnezia::RouteMode mode)
{
#ifdef AMNEZIA_DESKTOP
    if (!m_appSettingsRepository || !m_serversRepository) {
        return;
    }

    // Defense in depth: WG-family protocols manage their own split-tunnel DNS
    // inside the daemon. Calling the legacy IpcServer path here would set the
    // system resolver from the client side too, racing the daemon and breaking
    // post-handshake DATA exchange.
    const DockerContainer container = defaultContainerForServer(m_serversRepository, m_serversRepository->defaultServerId());
    if (ContainerUtils::isAwgContainer(container) ||
        container == DockerContainer::WireGuard) {
        return;
    }

    QStringList rules;
    const QVariantMap &sites = m_appSettingsRepository->vpnSites(mode);
    for (auto i = sites.constBegin(); i != sites.constEnd(); ++i) {
        const amnezia::SplitTunnelRule rule = amnezia::SplitTunnelRule::fromText(i.key());
        if (rule.isValid() && rule.type() != amnezia::SplitTunnelRule::Type::IpSubnet) {
            rules.append(rule.normalizedText());
        }
    }
    rules.removeDuplicates();

    const bool syncKillSwitch = mode == amnezia::RouteMode::VpnAllExceptSites &&
                                m_appSettingsRepository->isKillSwitchEnabled();
    qInfo() << "VpnConnection::configureDnsSplitTunnel mode=" << mode
            << "gateway=" << gw
            << "rules=" << rules.count()
            << "killswitch_sync=" << syncKillSwitch;
    setConnectionHealth(ConnectionHealth::CheckingDns);
    IpcClient::withInterface([this, rules, gw, syncKillSwitch](QSharedPointer<IpcInterfaceReplica> iface) {
        IpcClient::async(this, iface->configureDnsSplitTunnel(rules, gw, syncKillSwitch),
            [this](bool ok) {
                if (!ok) {
                    setConnectionHealth(ConnectionHealth::DnsFailed);
                    qWarning() << "VpnConnection::configureDnsSplitTunnel: Failed to configure DNS split tunnel";
                }
            });
    });
#else
    Q_UNUSED(gw);
    Q_UNUSED(mode);
#endif
}

bool VpnConnection::addSplitTunnelRoutes(const QString &gw, amnezia::RouteMode mode, const QStringList &ips)
{
#ifdef AMNEZIA_DESKTOP
    if (ips.isEmpty()) {
        return true;
    }

    bool ok = false;
    setConnectionHealth(ConnectionHealth::ApplyingRoutes);
    IpcClient::withInterface([this, gw, mode, ips, &ok](QSharedPointer<IpcInterfaceReplica> iface) {
        auto routeReply = iface->routeAddList(gw, ips);
        ok = routeReply.waitForFinished() && routeReply.returnValue() == ips.count();
        if (!ok) {
            setConnectionHealth(ConnectionHealth::RouteMismatch);
        }

        const QStringList allowedIps = amnezia::SplitTunnelRoutePlanner::killSwitchAllowedIps(
            mode, m_appSettingsRepository && m_appSettingsRepository->isKillSwitchEnabled(), ips);
        if (!allowedIps.isEmpty()) {
            setConnectionHealth(ConnectionHealth::CheckingFirewall);
            auto allowReply = iface->addKillSwitchAllowedRange(allowedIps);
            if (!allowReply.waitForFinished() || !allowReply.returnValue()) {
                setConnectionHealth(ConnectionHealth::FirewallBlocked);
                ok = false;
                qWarning() << "VpnConnection::addSplitTunnelRoutes: Failed to update killswitch allowed ranges";
            }
        }
    });
    return ok;
#else
    Q_UNUSED(gw);
    Q_UNUSED(mode);
    Q_UNUSED(ips);
    return true;
#endif
}

void VpnConnection::refreshSiteSplitTunnelRoutes()
{
    if (m_connectionState != Vpn::ConnectionState::Connected) {
        return;
    }
    if (!m_appSettingsRepository) {
        return;
    }
    const amnezia::RouteMode mode = m_appSettingsRepository->routeMode();
    const QString gw = (mode == amnezia::RouteMode::VpnAllExceptSites)
        ? (m_vpnProtocol ? m_vpnProtocol->routeGateway() : QString())
        : (m_vpnProtocol ? m_vpnProtocol->vpnGateway() : QString());
    configureDnsSplitTunnel(gw, mode);
    addSitesRoutes(gw, mode);
}

QSharedPointer<VpnProtocol> VpnConnection::vpnProtocol() const
{
    return m_vpnProtocol;
}

void VpnConnection::disconnectSlots()
{
    if (m_vpnProtocol) {
        m_vpnProtocol->disconnect();
    }
}

ErrorCode VpnConnection::lastError() const
{
#ifdef Q_OS_ANDROID
    return ErrorCode::AndroidError;
#endif

    if (m_lastError != ErrorCode::NoError) {
        return m_lastError;
    }

    const ErrorCode protocolError = m_vpnProtocol.isNull()
        ? ErrorCode::NoError
        : m_vpnProtocol.data()->lastError();
    if (protocolError != ErrorCode::NoError) {
        return protocolError;
    }

    if (m_connectionState == Vpn::ConnectionState::Error) {
        return ErrorCode::UnknownError;
    }

    return ErrorCode::NoError;
}

Vpn::ConnectionState VpnConnection::connectionState() const
{
    return m_connectionState;
}

ConnectionHealth VpnConnection::connectionHealth() const
{
    return m_connectionHealth;
}

void VpnConnection::connectToVpn(const QString &serverId, DockerContainer container, const QJsonObject &vpnConfiguration)
{
    if (!m_appSettingsRepository || !m_serversRepository) {
        qCritical() << "VpnConnection::connectToVpn: repositories not initialized";
        setConnectionState(Vpn::ConnectionState::Error);
        return;
    }

    qDebug() << QString("Trying to connect to VPN, server id is %1, container is %2, route mode is")
                        .arg(serverId)
                        .arg(ContainerUtils::containerToString(container))
             << m_appSettingsRepository->routeMode();

    markLastError(ErrorCode::NoError);
    m_connectedRecoveryAttempts = 0;
    m_noTrafficRecoveryAttempted = false;
    stopConnectedRecovery();
    m_stoppingAfterFailure = false;
    m_currentServerId = serverId;
    m_currentContainer = container;
    m_remoteAddress = NetworkUtilities::getIPAddress(vpnConfiguration.value(configKey::hostName).toString());
    stopPendingNetworkAction();
    connectServiceSignals();
    setConnectionState(Vpn::ConnectionState::Connecting);

    m_vpnConfiguration = vpnConfiguration;

#ifdef AMNEZIA_DESKTOP
    if (m_vpnProtocol) {
        disconnect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::onProtocolError);
        m_vpnProtocol->stop();
        m_vpnProtocol.reset();
    }
    appendKillSwitchConfig();
#endif

    appendSplitTunnelingConfig();

    if (deferUntilPhysicalNetworkReady(PendingNetworkAction::StartConnection,
                                       ConnectionHealth::CheckingLocalNetwork)) {
        return;
    }

    startConfiguredConnection();
}

void VpnConnection::startConfiguredConnection()
{
    stopPendingNetworkAction();
    startConnectingWatchdog();

    QSharedPointer<VpnProtocol> protocol;

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    protocol.reset(VpnProtocol::factory(m_currentContainer, m_vpnConfiguration));
    m_vpnProtocol = protocol;
    if (!protocol) {
        markLastError(ErrorCode::InternalError);
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(ErrorCode::InternalError);
        return;
    }
    protocol->prepare();
#elif defined Q_OS_ANDROID
    androidVpnProtocol = createDefaultAndroidVpnProtocol();
    createAndroidConnections();

    m_vpnProtocol.reset(androidVpnProtocol);
    protocol = m_vpnProtocol;
#elif defined Q_OS_IOS || defined(MACOS_NE)
    Proto proto = ContainerUtils::defaultProtocol(m_currentContainer);
    IosController::Instance()->connectVpn(proto, m_vpnConfiguration);
    connect(&m_checkTimer, &QTimer::timeout, IosController::Instance(), &IosController::checkStatus);
    return;
#endif

    createProtocolConnections();

    if (ErrorCode err = protocol->start(); err != ErrorCode::NoError) {
        if (m_vpnProtocol != protocol) {
            return;
        }
        markLastError(err);
        setConnectionHealth(ConnectionHealth::ProtocolStartFailed);
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(err);
        return;
    }

    if (m_vpnProtocol != protocol) {
        return;
    }
    if (!m_silentReconnectInProgress) {
        setConnectionHealth(ConnectionHealth::WaitingHandshake);
    }
}

void VpnConnection::createProtocolConnections()
{
    connect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::onProtocolError);
    connect(m_vpnProtocol.data(), &VpnProtocol::connectionStateChanged, this, &VpnConnection::setConnectionState);
    connect(m_vpnProtocol.data(), SIGNAL(bytesChanged(quint64, quint64)), this, SLOT(onBytesChanged(quint64, quint64)));
}

#ifdef AMNEZIA_DESKTOP
void VpnConnection::connectServiceSignals()
{
    IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> rep) {
        const auto uniqueQueued = static_cast<Qt::ConnectionType>(int(Qt::QueuedConnection) | int(Qt::UniqueConnection));
        connect(rep.data(), &IpcInterfaceReplica::networkChanged, this, &VpnConnection::reconnectToVpn, uniqueQueued);
        connect(rep.data(), &IpcInterfaceReplica::wakeup, this, &VpnConnection::reconnectToVpn, uniqueQueued);
        connect(rep.data(), &IpcInterfaceReplica::physicalNetworkReadyChanged,
                this, &VpnConnection::onPhysicalNetworkReadyChanged, uniqueQueued);
    });
}
#else
void VpnConnection::connectServiceSignals() {}
#endif

void VpnConnection::appendKillSwitchConfig()
{
    if (!m_appSettingsRepository) {
        qCritical() << "VpnConnection::appendKillSwitchConfig: repositories not initialized";
        return;
    }

    m_vpnConfiguration.insert(configKey::killSwitchOption, QVariant(m_appSettingsRepository->isKillSwitchEnabled()).toString());
    m_vpnConfiguration.insert(configKey::allowedDnsServers, QVariant(m_appSettingsRepository->getAllowedDnsServers()).toJsonValue());
}

void VpnConnection::appendSplitTunnelingConfig()
{
    if (!m_appSettingsRepository) {
        qCritical() << "VpnConnection::appendSplitTunnelingConfig: repositories not initialized";
        return;
    }

    bool allowSiteBasedSplitTunneling = true;

    // this block is for old native configs and for old self-hosted configs
    auto protocolName = ProtocolUtils::protoToString(ContainerUtils::defaultProtocol(m_currentContainer));
    if (protocolName == ProtocolUtils::protoToString(Proto::Awg) || protocolName == ProtocolUtils::protoToString(Proto::WireGuard)) {
        allowSiteBasedSplitTunneling = false;
        auto configData = m_vpnConfiguration.value(protocolName + "_config_data").toObject();
        if (configData.value(configKey::allowedIps).isString()) {
            QJsonArray allowedIpsJsonArray = QJsonArray::fromStringList(configData.value(configKey::allowedIps).toString().split(", "));
            configData.insert(configKey::allowedIps, allowedIpsJsonArray);
            m_vpnConfiguration.insert(protocolName + "_config_data", configData);
        } else if (configData.value(configKey::allowedIps).isUndefined()) {
            auto nativeConfig = configData.value(configKey::config).toString();
            auto nativeConfigLines = nativeConfig.split("\n");
            for (auto &line : nativeConfigLines) {
                auto allowedIpsString = line.split("=", Qt::KeepEmptyParts);
                if (allowedIpsString.size() >= 2 && allowedIpsString.first().trimmed() == QStringLiteral("AllowedIPs")) {
                    QJsonArray allowedIpsJsonArray;
                    const QString allowedIps = allowedIpsString.mid(1).join(QStringLiteral("=")).trimmed();
                    for (const QString &allowedIp : allowedIps.split(",", Qt::SkipEmptyParts)) {
                        allowedIpsJsonArray.append(allowedIp.trimmed());
                    }
                    configData.insert(configKey::allowedIps, allowedIpsJsonArray);
                    m_vpnConfiguration.insert(protocolName + "_config_data", configData);
                    break;
                }
            }
        }

        if (configData.value(configKey::persistentKeepAlive).isUndefined()) {
            auto nativeConfig = configData.value(configKey::config).toString();
            auto nativeConfigLines = nativeConfig.split("\n");
            for (auto &line : nativeConfigLines) {
                auto persistentKeepaliveString = line.split("=", Qt::KeepEmptyParts);
                if (persistentKeepaliveString.size() >= 2
                        && persistentKeepaliveString.first().trimmed() == QStringLiteral("PersistentKeepalive")) {
                    configData.insert(configKey::persistentKeepAlive,
                                      persistentKeepaliveString.mid(1).join(QStringLiteral("=")).trimmed());
                    m_vpnConfiguration.insert(protocolName + "_config_data", configData);
                    break;
                }
            }
        }

        QJsonArray allowedIpsJsonArray = configData.value(configKey::allowedIps).toArray();
        if (allowedIpsJsonArray.contains("0.0.0.0/0") && allowedIpsJsonArray.contains("::/0")) {
            allowSiteBasedSplitTunneling = true;
        }
    }

    amnezia::RouteMode routeMode = amnezia::RouteMode::VpnAllSites;
    QJsonArray sitesJsonArray;
    QJsonArray dnsRulesJsonArray;
    if (m_appSettingsRepository->isSitesSplitTunnelingEnabled()) {
        routeMode = m_appSettingsRepository->routeMode();

        if (allowSiteBasedSplitTunneling) {
            QStringList sites;
            QStringList dnsRules;
            bool hasDynamicHostRules = false;
            const QVariantMap &m = m_appSettingsRepository->vpnSites(routeMode);
            for (auto i = m.constBegin(); i != m.constEnd(); ++i) {
                const QString &ruleText = i.key();
                const amnezia::SplitTunnelRule rule = amnezia::SplitTunnelRule::fromText(ruleText);
                if (!rule.isValid()) {
                    continue;
                }
                if (rule.type() == amnezia::SplitTunnelRule::Type::IpSubnet) {
                    sites.append(rule.normalizedText());
                    continue;
                }
                for (const QString &ip : SecureAppSettingsRepository::siteIpList(i.value())) {
                    if (NetworkUtilities::checkIpSubnetFormat(ip)) {
                        sites.append(ip);
                    }
                }
                if (rule.isDynamicHostRule()) {
                    hasDynamicHostRules = true;
                }
                // Hostname / wildcard rules are forwarded to the daemon so it
                // can resolve them periodically and add exclusion routes for
                // every IPv4 the host currently maps to. Without this, only
                // pre-resolved IP entries from the settings store reach the
                // daemon, and a rule like "kinopoisk.ru" never gets routed.
                if (rule.type() != amnezia::SplitTunnelRule::Type::IpSubnet) {
                    dnsRules.append(rule.normalizedText());
                }
            }
            sites.removeDuplicates();
            dnsRules.removeDuplicates();
            for (const auto &site : sites) {
                sitesJsonArray.append(site);
            }
            for (const auto &rule : dnsRules) {
                dnsRulesJsonArray.append(rule);
            }

            if (sitesJsonArray.isEmpty() && !hasDynamicHostRules && dnsRulesJsonArray.isEmpty()) {
                routeMode = amnezia::RouteMode::VpnAllSites;
            } else if (routeMode == amnezia::RouteMode::VpnOnlyForwardSites) {
                // Allow traffic to Amnezia DNS
                sitesJsonArray.append(m_vpnConfiguration.value(configKey::dns1).toString());
                sitesJsonArray.append(m_vpnConfiguration.value(configKey::dns2).toString());
            }
        }
    }

    m_vpnConfiguration.insert(configKey::splitTunnelType, routeMode);
    m_vpnConfiguration.insert(configKey::splitTunnelSites, sitesJsonArray);
    m_vpnConfiguration.insert(configKey::splitTunnelDnsRules, dnsRulesJsonArray);

    amnezia::AppsRouteMode appsRouteMode = amnezia::AppsRouteMode::VpnAllApps;
    QJsonArray appsJsonArray;
    if (m_appSettingsRepository->isAppsSplitTunnelingEnabled()) {
        appsRouteMode = m_appSettingsRepository->appsRouteMode();

        auto apps = m_appSettingsRepository->vpnApps(appsRouteMode);
        for (const auto &app : apps) {
            appsJsonArray.append(app.appPath.isEmpty() ? app.packageName : app.appPath);
        }

        if (appsJsonArray.isEmpty()) {
            appsRouteMode = amnezia::AppsRouteMode::VpnAllApps;
        }
    }

    m_vpnConfiguration.insert(configKey::appSplitTunnelType, appsRouteMode);
    m_vpnConfiguration.insert(configKey::splitTunnelApps, appsJsonArray);

    qDebug() << QString("Site split tunneling is %1, route mode is %2")
                        .arg(m_appSettingsRepository->isSitesSplitTunnelingEnabled() ? "enabled" : "disabled")
                        .arg(routeMode);
    qDebug() << QString("App split tunneling is %1, route mode is %2")
                        .arg(m_appSettingsRepository->isAppsSplitTunnelingEnabled() ? "enabled" : "disabled")
                        .arg(appsRouteMode);
}

#ifdef Q_OS_ANDROID
void VpnConnection::restoreConnection()
{
    createAndroidConnections();

    m_vpnProtocol.reset(androidVpnProtocol);

    createProtocolConnections();
}

void VpnConnection::createAndroidConnections()
{
    androidVpnProtocol = createDefaultAndroidVpnProtocol();

    connect(AndroidController::instance(), &AndroidController::connectionStateChanged, androidVpnProtocol,
            &AndroidVpnProtocol::setConnectionState);
    connect(AndroidController::instance(), &AndroidController::statisticsUpdated, androidVpnProtocol, &AndroidVpnProtocol::setBytesChanged);
}

AndroidVpnProtocol *VpnConnection::createDefaultAndroidVpnProtocol()
{
    return new AndroidVpnProtocol(m_vpnConfiguration);
}
#endif

QString VpnConnection::bytesPerSecToText(quint64 bytes)
{
    double mbps = bytes * 8 / 1e6;
    return QString("%1 %2").arg(QString::number(mbps, 'f', 2)).arg(tr("Mbps")); // Mbit/s
}

bool VpnConnection::physicalNetworkReady() const
{
#if defined(AMNEZIA_DESKTOP) && defined(Q_OS_MACOS)
    bool queryCompleted = false;
    bool ready = true;
    IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
        auto reply = iface->physicalNetworkReady();
        if (reply.waitForFinished()) {
            ready = reply.returnValue();
            queryCompleted = true;
        }
    });
    if (!queryCompleted) {
        qWarning() << "Unable to query physical network readiness; "
                      "continuing so the local service can report its own error";
    }
    return ready;
#else
    return true;
#endif
}

bool VpnConnection::deferUntilPhysicalNetworkReady(
    PendingNetworkAction action, ConnectionHealth health)
{
    if (physicalNetworkReady()) {
        return false;
    }

    m_pendingNetworkAction = action;
    m_pendingNetworkTimer.start();
    stopConnectingWatchdog();
    stopConnectedRecovery();
    stopConnectivityProbe();
    setConnectionHealth(health);
    qInfo() << "Waiting for a physical default route before VPN start";
    QMetaObject::invokeMethod(this, &VpnConnection::resumePendingNetworkAction,
                              Qt::QueuedConnection);
    return true;
}

void VpnConnection::onPhysicalNetworkReadyChanged(bool ready)
{
    if (ready) {
        resumePendingNetworkAction();
    }
}

void VpnConnection::resumePendingNetworkAction()
{
    if (m_pendingNetworkAction == PendingNetworkAction::None ||
        !physicalNetworkReady()) {
        return;
    }

    const PendingNetworkAction action = m_pendingNetworkAction;
    stopPendingNetworkAction();
    qInfo() << "Physical default route is ready; resuming VPN start";

    switch (action) {
    case PendingNetworkAction::StartConnection:
        if (m_connectionState == Vpn::ConnectionState::Connecting) {
            startConfiguredConnection();
        }
        break;
    case PendingNetworkAction::RequestReconnect:
        if (m_connectionState == Vpn::ConnectionState::Connected) {
            reconnectToVpn();
        }
        break;
    case PendingNetworkAction::StartSilentReconnect:
        if (m_connectionState == Vpn::ConnectionState::Connected) {
            startSilentReconnect();
        }
        break;
    case PendingNetworkAction::None:
        break;
    }
}

void VpnConnection::reconnectToVpn() {
    if (m_vpnProtocol.isNull())
        return;

    if (m_silentReconnectInProgress ||
        m_connectedRecoveryTimer.isActive() ||
        m_pendingNetworkAction != PendingNetworkAction::None) {
        qDebug() << "Reconnect request coalesced with an active recovery";
        return;
    }

    if (m_connectionState != Vpn::ConnectionState::Connected) {
        qWarning() << QString("Reconnect triggered on %1 during inappropriate state: %2; ignoring slot")
                              .arg(QMetaEnum::fromType<Vpn::ConnectionState>().valueToKey(m_connectionState));
        return;
    }

    if (deferUntilPhysicalNetworkReady(PendingNetworkAction::RequestReconnect,
                                       ConnectionHealth::Recovering)) {
        return;
    }

    if (!m_silentReconnectInProgress) {
        m_connectedRecoveryAttempts = 0;
    }
    if (m_connectedRecoveryAttempts >= kMaxConnectedRecoveryAttempts) {
        qWarning() << "Automatic reconnect limit reached; ignoring reconnect request";
        return;
    }

    ++m_connectedRecoveryAttempts;
    m_noTrafficRecoveryAttempted = true;

    qWarning().noquote()
        << "AmneziaDiagnostic event=auto_recover"
        << "diagnostic=" + QString::number(static_cast<int>(m_connectionHealth))
        << "diagnostic_text=" + connectionHealthText(m_connectionHealth)
        << "attempt=" + QString::number(m_connectedRecoveryAttempts)
        << "max_attempts=" + QString::number(kMaxConnectedRecoveryAttempts);

    startSilentReconnect();
}

void VpnConnection::startSilentReconnect()
{
    if (m_silentReconnectInProgress) {
        qDebug() << "Silent reconnect already in progress";
        return;
    }
    if (deferUntilPhysicalNetworkReady(
            PendingNetworkAction::StartSilentReconnect,
            ConnectionHealth::Recovering)) {
        return;
    }

    stopPendingNetworkAction();
    qDebug() << "Reconnect triggered. Reconnecting to the server";

    stopConnectingWatchdog();
    stopConnectedRecovery();
    m_silentReconnectInProgress = true;
    setConnectionHealth(ConnectionHealth::Recovering);

    disconnect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::onProtocolError);
    disconnect(m_vpnProtocol.data(), &VpnProtocol::connectionStateChanged, this, &VpnConnection::setConnectionState);
    disconnect(m_vpnProtocol.data(), SIGNAL(bytesChanged(quint64, quint64)), this, SLOT(onBytesChanged(quint64, quint64)));
    m_vpnProtocol->stop();
    QSharedPointer<VpnProtocol> protocol(VpnProtocol::factory(m_currentContainer, m_vpnConfiguration));
    m_vpnProtocol = protocol;
    if (!protocol) {
        m_silentReconnectInProgress = false;
        markLastError(ErrorCode::InternalError);
        setConnectionHealth(ConnectionHealth::ProtocolStartFailed);
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(ErrorCode::InternalError);
        return;
    }
    createProtocolConnections();

    startConnectingWatchdog();
    if (ErrorCode err = protocol->start(); err != ErrorCode::NoError) {
        if (m_vpnProtocol != protocol) {
            return;
        }
        if (recoverableProtocolError(err)) {
            handleSilentReconnectFailure(connectionHealthForError(err), err);
            return;
        }
        stopConnectingWatchdog();
        m_silentReconnectInProgress = false;
        markLastError(err);
        setConnectionHealth(ConnectionHealth::ProtocolStartFailed);
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(err);
        return;
    }

    if (m_vpnProtocol != protocol) {
        return;
    }
    if (m_silentReconnectInProgress) {
        setConnectionHealth(ConnectionHealth::Recovering);
    }
}

void VpnConnection::disconnectFromVpn()
{
#if defined(Q_OS_IOS) || defined(MACOS_NE)
    // iOS/macOS NE use IosController directly; m_vpnProtocol is not set there.
    IosController::Instance()->disconnectVpn();
    disconnect(&m_checkTimer, &QTimer::timeout, IosController::Instance(), &IosController::checkStatus);
#endif

    stopPendingNetworkAction();
    m_silentReconnectInProgress = false;
    m_stoppingAfterFailure = false;
    m_connectedRecoveryAttempts = 0;
    m_noTrafficRecoveryAttempted = false;
    stopConnectedRecovery();
    stopConnectingWatchdog();

    if (m_vpnProtocol.isNull()) {
        setConnectionState(Vpn::ConnectionState::Disconnected);
        return;
    }

    setConnectionState(Vpn::ConnectionState::Disconnecting);

#ifdef Q_OS_ANDROID
    auto *const connection = new QMetaObject::Connection;
    *connection = connect(AndroidController::instance(), &AndroidController::vpnStateChanged, this,
                          [this, connection](AndroidController::ConnectionState state) {
                              if (state == AndroidController::ConnectionState::DISCONNECTED) {
                                  setConnectionState(Vpn::ConnectionState::Disconnected);
                                  disconnect(*connection);
                                  delete connection;
                              }
                          });
#endif

    m_vpnProtocol->stop();

#if !defined(Q_OS_ANDROID) && !defined(AMNEZIA_DESKTOP)
    m_vpnProtocol->deleteLater();
#endif

    m_vpnProtocol = nullptr;
    if (m_connectionState == Vpn::ConnectionState::Disconnecting) {
        setConnectionState(Vpn::ConnectionState::Disconnected);
    }
}

void VpnConnection::setConnectionState(Vpn::ConnectionState state) {
    const Vpn::ConnectionState previousState = m_connectionState;

    if (m_silentReconnectInProgress &&
        state == Vpn::ConnectionState::Disconnected &&
        previousState == Vpn::ConnectionState::Connected) {
        handleSilentReconnectFailure(ConnectionHealth::LocalServiceUnavailable,
                                     ErrorCode::AmneziaServiceConnectionFailed);
        return;
    }
    if (m_stoppingAfterFailure &&
        state == Vpn::ConnectionState::Disconnected) {
        return;
    }
    if (previousState == Vpn::ConnectionState::Error &&
        state == Vpn::ConnectionState::Disconnected &&
        m_lastError != ErrorCode::NoError) {
        return;
    }

    if (state == Vpn::ConnectionState::Connected) {
        m_silentReconnectInProgress = false;
    }

    if (state == Vpn::ConnectionState::Error &&
        m_lastError == ErrorCode::NoError &&
        (m_vpnProtocol.isNull() ||
         m_vpnProtocol.data()->lastError() == ErrorCode::NoError)) {
        markLastError(ErrorCode::UnknownError);
    }

    if (state == Vpn::Disconnected && previousState == Vpn::Reconnecting) {
        m_connectionState = state;
        onConnectionStateChanged(state);
        m_connectionState = previousState;
        return;
    }

    m_connectionState = state;
    onConnectionStateChanged(state);
    emit connectionStateChanged(state);
}

void VpnConnection::onProtocolError(ErrorCode error)
{
    if (m_silentReconnectInProgress && recoverableProtocolError(error)) {
        handleSilentReconnectFailure(connectionHealthForError(error), error);
        return;
    }

    markLastError(error);
    if (error == ErrorCode::AmneziaServiceConnectionFailed ||
        error == ErrorCode::VpnBackendFailure) {
        setConnectionHealth(ConnectionHealth::LocalServiceUnavailable);
    }
    if (m_connectionState != Vpn::ConnectionState::Disconnected &&
        m_connectionState != Vpn::ConnectionState::Disconnecting) {
        m_silentReconnectInProgress = false;
        setConnectionState(Vpn::ConnectionState::Error);
    }
    emit vpnProtocolError(error);
}

void VpnConnection::onConnectingTimedOut()
{
    const bool silentReconnectTimedOut =
        m_silentReconnectInProgress &&
        m_connectionState == Vpn::ConnectionState::Connected;
    if (!silentReconnectTimedOut &&
        m_connectionState != Vpn::ConnectionState::Connecting &&
        m_connectionState != Vpn::ConnectionState::Reconnecting) {
        return;
    }

    if (silentReconnectTimedOut) {
        handleSilentReconnectFailure(ConnectionHealth::HandshakeTimeout,
                                     ErrorCode::VpnHandshakeTimeout);
        return;
    }

    markLastError(ErrorCode::VpnHandshakeTimeout);
    setConnectionHealth(ConnectionHealth::HandshakeTimeout);
    if (!m_vpnProtocol.isNull()) {
        m_stoppingAfterFailure = true;
        m_vpnProtocol->stop();
    }
    setConnectionState(Vpn::ConnectionState::Error);
    m_stoppingAfterFailure = false;
    emit vpnProtocolError(ErrorCode::VpnHandshakeTimeout);
}

bool VpnConnection::handleSilentReconnectFailure(ConnectionHealth health, ErrorCode error)
{
    stopConnectingWatchdog();
    m_silentReconnectInProgress = false;

    if (!m_vpnProtocol.isNull()) {
        m_stoppingAfterFailure = true;
        disconnect(m_vpnProtocol.data(), &VpnProtocol::protocolError,
                   this, &VpnConnection::onProtocolError);
        disconnect(m_vpnProtocol.data(), &VpnProtocol::connectionStateChanged,
                   this, &VpnConnection::setConnectionState);
        disconnect(m_vpnProtocol.data(), SIGNAL(bytesChanged(quint64, quint64)),
                   this, SLOT(onBytesChanged(quint64, quint64)));
        m_vpnProtocol->stop();
        m_stoppingAfterFailure = false;
    }

    if (recoverConnectedTunnel(health, error)) {
        return true;
    }

    setConnectionState(Vpn::ConnectionState::Error);
    emit vpnProtocolError(error);
    return false;
}

void VpnConnection::checkConnectedHealth()
{
    if (m_connectionState != Vpn::ConnectionState::Connected) {
        stopConnectedHealthCheck();
        return;
    }
    if (m_silentReconnectInProgress) {
        return;
    }
    checkDnsHealth();
    if (m_connectionHealth == ConnectionHealth::DnsFailed) {
        return;
    }
    if (m_connectionHealth == ConnectionHealth::Healthy) {
        startConnectivityProbe();
        return;
    }
    if (m_vpnProtocol.isNull()) {
        startConnectivityProbe();
        return;
    }

    const bool statusRequested = m_vpnProtocol->requestStatus();
    if (!statusRequested) {
        startConnectivityProbe();
        return;
    }

    ++m_healthChecksWithoutTraffic;
    if (m_healthChecksWithoutTraffic >= 5) {
        startConnectivityProbe();
    }
}

void VpnConnection::onConnectivityProbeSucceeded()
{
    if (m_connectionState != Vpn::ConnectionState::Connected) {
        stopConnectivityProbe();
        return;
    }

    stopConnectivityProbe();
    markLastError(ErrorCode::NoError);
    m_connectedRecoveryAttempts = 0;
    m_noTrafficRecoveryAttempted = false;
    if (m_connectionHealth != ConnectionHealth::DnsFailed) {
        setConnectionHealth(ConnectionHealth::Healthy);
    }
}

void VpnConnection::onConnectivityProbeFailed()
{
    if (m_connectionState != Vpn::ConnectionState::Connected) {
        stopConnectivityProbe();
        return;
    }

    stopConnectivityProbe();
    if (m_silentReconnectInProgress) {
        return;
    }
    recoverConnectedTunnel(ConnectionHealth::NoTraffic);
}

void VpnConnection::setConnectionHealth(ConnectionHealth health)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, health]() {
            setConnectionHealth(health);
        }, Qt::QueuedConnection);
        return;
    }

    if (!shouldPublishConnectionHealth(health)) {
        return;
    }
    if (m_connectionHealth == health) {
        return;
    }

    const ConnectionHealth previousHealth = m_connectionHealth;
    m_connectionHealth = health;

    const QString protocolName = m_vpnProtocol
        ? QString::fromLatin1(m_vpnProtocol->metaObject()->className())
        : QStringLiteral("none");
    qInfo().noquote()
        << "AmneziaDiagnostic event=state_change"
        << "connection_state=" + QString::number(static_cast<int>(m_connectionState))
        << "previous_diagnostic=" + QString::number(static_cast<int>(previousHealth))
        << "diagnostic=" + QString::number(static_cast<int>(health))
        << "diagnostic_text=" + connectionHealthText(health)
        << "last_error=" + QString::number(static_cast<int>(m_lastError))
        << "protocol=" + protocolName
        << "recovery_attempted=" + QString(m_noTrafficRecoveryAttempted ? "true" : "false");

    emit connectionHealthChanged(health);
}

bool VpnConnection::shouldPublishConnectionHealth(ConnectionHealth health) const
{
    if (m_connectionState == Vpn::ConnectionState::Disconnected) {
        return health == ConnectionHealth::Idle ||
               (m_lastError != ErrorCode::NoError && connectionHealthProblem(health));
    }
    if (m_connectionState == Vpn::ConnectionState::Error) {
        return connectionHealthProblem(health);
    }
    return true;
}

void VpnConnection::startConnectingWatchdog()
{
    m_connectingTimer.start();
}

void VpnConnection::stopConnectingWatchdog()
{
    m_connectingTimer.stop();
}

void VpnConnection::stopPendingNetworkAction()
{
    m_pendingNetworkAction = PendingNetworkAction::None;
    m_pendingNetworkTimer.stop();
}

void VpnConnection::startConnectedHealthCheck()
{
    m_healthChecksWithoutTraffic = 0;
    if (!connectionHealthProblem(m_connectionHealth)) {
        setConnectionHealth(ConnectionHealth::CheckingTraffic);
    }
    m_healthTimer.start();
    QTimer::singleShot(0, this, [this]() {
        checkConnectedHealth();
        startConnectivityProbe();
    });
}

void VpnConnection::stopConnectedHealthCheck()
{
    m_healthTimer.stop();
    m_healthChecksWithoutTraffic = 0;
}

void VpnConnection::stopConnectedRecovery()
{
    m_connectedRecoveryTimer.stop();
}

void VpnConnection::checkDnsHealth()
{
    QHostInfo::lookupHost(QStringLiteral("cloudflare.com"), this, [this](const QHostInfo &hostInfo) {
        if (m_connectionState != Vpn::ConnectionState::Connected) {
            return;
        }
        if (m_silentReconnectInProgress) {
            return;
        }
        if (hostInfo.error() != QHostInfo::NoError || hostInfo.addresses().isEmpty()) {
            recoverConnectedTunnel(ConnectionHealth::DnsFailed);
            return;
        }
        if (m_connectionHealth == ConnectionHealth::DnsFailed) {
            markLastError(ErrorCode::NoError);
            setConnectionHealth(ConnectionHealth::Healthy);
        }
    });
}

bool VpnConnection::recoverConnectedTunnel(ConnectionHealth health, ErrorCode error)
{
    markLastError(error);
    setConnectionHealth(health);

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(MACOS_NE)
    stopConnectedHealthCheck();
    return false;
#else
    if (m_connectionState != Vpn::ConnectionState::Connected ||
        m_silentReconnectInProgress ||
        m_vpnProtocol.isNull() ||
        m_currentServerId.isEmpty()) {
        stopConnectedHealthCheck();
        return false;
    }

    if (m_connectedRecoveryTimer.isActive() ||
        m_pendingNetworkAction != PendingNetworkAction::None) {
        return true;
    }

    if (m_connectedRecoveryAttempts >= kMaxConnectedRecoveryAttempts) {
        stopConnectedHealthCheck();
        qWarning().noquote()
            << "AmneziaDiagnostic event=auto_recover_exhausted"
            << "diagnostic=" + QString::number(static_cast<int>(health))
            << "diagnostic_text=" + connectionHealthText(health)
            << "attempts=" + QString::number(m_connectedRecoveryAttempts);
        return false;
    }

    ++m_connectedRecoveryAttempts;
    m_noTrafficRecoveryAttempted = true;
    stopConnectedHealthCheck();
    stopConnectivityProbe();

    qWarning().noquote()
        << "AmneziaDiagnostic event=auto_recover"
        << "diagnostic=" + QString::number(static_cast<int>(health))
        << "diagnostic_text=" + connectionHealthText(health)
        << "attempt=" + QString::number(m_connectedRecoveryAttempts)
        << "max_attempts=" + QString::number(kMaxConnectedRecoveryAttempts);

    m_connectedRecoveryTimer.start();
    return true;
#endif
}

void VpnConnection::startConnectivityProbe()
{
    if (m_connectivityProbe) {
        return;
    }

    auto *socket = new QTcpSocket(this);
    m_connectivityProbe.reset(socket);

    connect(socket, &QTcpSocket::connected, this, [this]() {
        qInfo() << "Connectivity probe to 1.1.1.1:443 succeeded";
        onConnectivityProbeSucceeded();
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
        qWarning() << "Connectivity probe to 1.1.1.1:443 failed"
                   << error << socket->errorString();
        onConnectivityProbeFailed();
    });

    QPointer<QTcpSocket> probe(socket);
    QTimer::singleShot(5000, this, [this, probe]() {
        if (probe && m_connectivityProbe.get() == probe && probe->state() != QAbstractSocket::ConnectedState) {
            qWarning() << "Connectivity probe to 1.1.1.1:443 timed out";
            onConnectivityProbeFailed();
        }
    });

    connectConnectivityProbe(socket);
}

void VpnConnection::stopConnectivityProbe()
{
    if (!m_connectivityProbe) {
        return;
    }

    QTcpSocket *probe = m_connectivityProbe.release();
    probe->disconnect(this);
    probe->abort();
    probe->deleteLater();
}

void VpnConnection::connectConnectivityProbe(QTcpSocket *socket)
{
    socket->connectToHost(QStringLiteral("1.1.1.1"), 443);
}

void VpnConnection::markLastError(ErrorCode error)
{
    m_lastError = error;
}
