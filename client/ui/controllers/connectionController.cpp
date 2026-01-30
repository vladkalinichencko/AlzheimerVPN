#include "connectionController.h"

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(MACOS_NE)
    #include <QGuiApplication>
#else
    #include <QApplication>
#endif

#include "utilities.h"
#include "core/controllers/vpnConfigurationController.h"
#include "core/api/apiUtils.h"
#include "containers/containers_defs.h"
#include "protocols/protocols_defs.h"
#include "version.h"

namespace
{
    namespace configKey
    {
        constexpr char apiConfig[] = "api_config";
        constexpr char serviceProtocol[] = "service_protocol";
    }
}

ConnectionController::ConnectionController(const QSharedPointer<ServersModel> &serversModel,
                                           const QSharedPointer<ContainersModel> &containersModel,
                                           const QSharedPointer<ClientManagementModel> &clientManagementModel,
                                           const QSharedPointer<VpnConnection> &vpnConnection, const std::shared_ptr<Settings> &settings,
                                           QObject *parent)
    : QObject(parent),
      m_serversModel(serversModel),
      m_containersModel(containersModel),
      m_clientManagementModel(clientManagementModel),
      m_vpnConnection(vpnConnection),
      m_settings(settings)
{
    connect(m_vpnConnection.get(), &VpnConnection::connectionStateChanged, this, &ConnectionController::onConnectionStateChanged);
    connect(this, &ConnectionController::connectToVpn, m_vpnConnection.get(), &VpnConnection::connectToVpn, Qt::QueuedConnection);
    connect(this, &ConnectionController::disconnectFromVpn, m_vpnConnection.get(), &VpnConnection::disconnectFromVpn, Qt::QueuedConnection);

    connect(this, &ConnectionController::connectButtonClicked, this, &ConnectionController::toggleConnection, Qt::QueuedConnection);

    m_awgStateTimer.setSingleShot(true);
    connect(&m_awgStateTimer, &QTimer::timeout, this, &ConnectionController::onAwgStateTimeout);

    m_state = Vpn::ConnectionState::Disconnected;
}

void ConnectionController::openConnection()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    if (!Utils::processIsRunning(Utils::executable(SERVICE_NAME, false), true))
    {
        emit connectionErrorOccurred(ErrorCode::AmneziaServiceNotRunning);
        return;
    }
#endif

    int serverIndex = m_serversModel->getDefaultServerIndex();
    QJsonObject serverConfig = m_serversModel->getServerConfig(serverIndex);

    DockerContainer container = qvariant_cast<DockerContainer>(m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole));

    if (!m_containersModel->isSupportedByCurrentPlatform(container)) {
        emit connectionErrorOccurred(ErrorCode::NotSupportedOnThisPlatform);
        return;
    }

    QSharedPointer<ServerController> serverController(new ServerController(m_settings));
    VpnConfigurationsController vpnConfigurationController(m_settings, serverController);

    QJsonObject containerConfig = m_containersModel->getContainerConfig(container);
    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);

    auto dns = m_serversModel->getDnsPair(serverIndex);

    auto vpnConfiguration = vpnConfigurationController.createVpnConfiguration(dns, serverConfig, containerConfig, container);
    emit connectToVpn(serverIndex, credentials, container, vpnConfiguration);
}

void ConnectionController::closeConnection()
{
    emit disconnectFromVpn();
}

ErrorCode ConnectionController::getLastConnectionError()
{
    return m_vpnConnection->lastError();
}

void ConnectionController::onConnectionStateChanged(Vpn::ConnectionState state)
{
    m_state = state;

    m_isConnected = false;
    m_connectionStateText = tr("Connecting...");
    switch (state) {
    case Vpn::ConnectionState::Connected: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }

        m_isConnectionInProgress = false;
        m_isConnected = true;
        m_connectionStateText = tr("Connected");
        break;
    }
    case Vpn::ConnectionState::Connecting: {
        checkAndStartAwgStateTimer();
        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Reconnecting: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Reconnecting...");
        break;
    }
    case Vpn::ConnectionState::Disconnected: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        break;
    }
    case Vpn::ConnectionState::Disconnecting: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Disconnecting...");
        break;
    }
    case Vpn::ConnectionState::Preparing: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Preparing...");
        break;
    }
    case Vpn::ConnectionState::Error: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    case Vpn::ConnectionState::Unknown: {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    }
    emit connectionStateChanged();
}

void ConnectionController::onAwgStateTimeout()
{
    if (m_state != Vpn::ConnectionState::Connecting) {
        return;
    }

    const int serverIndex = m_serversModel->getDefaultServerIndex();
    if (serverIndex < 0) {
        return;
    }

    const QVariant containerVar = m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole);
    if (!containerVar.isValid()) {
        return;
    }

    const DockerContainer container = qvariant_cast<DockerContainer>(containerVar);
    const Proto proto = ContainerProps::defaultProtocol(container);
    if (proto != Proto::Awg) {
        return;
    }

    closeConnection();

    QTimer::singleShot(1000, this, [this, serverIndex]() {
        if (m_isConnected || m_isConnectionInProgress) {
            return;
        }

        qDebug().noquote()
            << "AWG connect timeout: trying to switch API protocol to VLESS"
            << "and reload config from gateway for premium server index" << serverIndex;

        // Store state for async operation
        m_pendingApiServerIndex = serverIndex;
        m_apiSwitched = false;
        m_waitingForApiUpdate = true;

        m_serversModel->setProcessedServerIndex(serverIndex);
        
        emit requestSetCurrentProtocol(QStringLiteral("vless"));
        emit requestUpdateServiceFromGateway(serverIndex, QString(), QString(), true);
        
        return;
    });
}

void ConnectionController::onCurrentContainerUpdated()
{
    if (m_isConnected || m_isConnectionInProgress) {
        emit reconnectWithUpdatedContainer(tr("Settings updated successfully, reconnnection..."));
        openConnection();
    } else {
        emit reconnectWithUpdatedContainer(tr("Settings updated successfully"));
    }
}

void ConnectionController::onTranslationsUpdated()
{
    // get translated text of current state
    onConnectionStateChanged(getCurrentConnectionState());
}

Vpn::ConnectionState ConnectionController::getCurrentConnectionState()
{
    return m_state;
}

QString ConnectionController::connectionStateText() const
{
    return m_connectionStateText;
}

void ConnectionController::toggleConnection()
{
    if (m_state == Vpn::ConnectionState::Preparing) {
        emit preparingConfig();
        return;
    }

    if (isConnectionInProgress()) {
        closeConnection();
    } else if (isConnected()) {
        closeConnection();
    } else {
        emit prepareConfig();
    }
}

bool ConnectionController::isConnectionInProgress() const
{
    return m_isConnectionInProgress;
}

bool ConnectionController::isConnected() const
{
    return m_isConnected;
}

void ConnectionController::onUpdateServiceFromGatewayCompleted(bool success, int serverIndex)
{
    if (!m_waitingForApiUpdate || m_pendingApiServerIndex != serverIndex) {
        return;
    }

    m_waitingForApiUpdate = false;
    m_apiSwitched = success;
    bool hasXray = false;

    if (success) {
        const QJsonObject newServerConfig = m_serversModel->getServerConfig(serverIndex);
        const QJsonArray newContainers = newServerConfig.value(config_key::containers).toArray();
        for (const QJsonValue &value : newContainers) {
            const QJsonObject obj = value.toObject();
            const DockerContainer c =
                    ContainerProps::containerFromString(obj.value(config_key::container).toString());
            if (c == DockerContainer::Xray) {
                hasXray = true;
                break;
            }
        }
    }

    if (!hasXray) {
        qDebug().noquote()
            << "AWG connect timeout: no XRay available for server index" << serverIndex
            << "(API switch attempt success =" << (m_apiSwitched ? "YES" : "NO") << ")";
        m_pendingApiServerIndex = -1;
        return;
    }

    qDebug().noquote() << "AWG connect timeout (10s), switching default container to XRay for server index"
                       << serverIndex << "and reconnecting";

    m_serversModel->setDefaultContainer(serverIndex, static_cast<int>(DockerContainer::Xray));

    m_serversModel->setProcessedServerIndex(serverIndex);
    emit requestSetCurrentProtocol(QStringLiteral("vless"));

    m_pendingApiServerIndex = -1;

    if (!m_isConnected && !m_isConnectionInProgress) {
        emit prepareConfig();
    }
}

void ConnectionController::checkAndStartAwgStateTimer()
{
    const int serverIndex = m_serversModel->getDefaultServerIndex();
    if (serverIndex < 0) {
        return;
    }

    const QVariant containerVar = m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole);
    if (!containerVar.isValid()) {
        return;
    }

    const DockerContainer container = qvariant_cast<DockerContainer>(containerVar);
    const Proto proto = ContainerProps::defaultProtocol(container);
    if (proto != Proto::Awg) {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
        return;
    }

    const QJsonObject serverConfig = m_serversModel->getServerConfig(serverIndex);

    // Auto-switching only works in auto mode, not when protocol is explicitly selected
    const QJsonObject apiConfig = serverConfig.value(configKey::apiConfig).toObject();
    const QString serviceProtocol = apiConfig.value(configKey::serviceProtocol).toString();
    // Auto mode: empty serviceProtocol means use default protocol (AWG for AWG container)
    const bool isAutoMode = serviceProtocol.isEmpty();
    
    if (isAutoMode && apiUtils::isPremiumServer(serverConfig)) {
        m_awgStateTimer.start(10000);
    } else {
        if (m_awgStateTimer.isActive()) {
            m_awgStateTimer.stop();
        }
    }
}
