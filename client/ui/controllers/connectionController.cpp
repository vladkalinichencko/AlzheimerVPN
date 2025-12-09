#include "connectionController.h"

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(MACOS_NE)
    #include <QGuiApplication>
#else
    #include <QApplication>
#endif

#include "utilities.h"
#include "amnezia_application.h"
#include "core/controllers/vpnConfigurationController.h"
#include "containers/containers_defs.h"
#include "protocols/protocols_defs.h"
#include "version.h"

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
        m_awgStateTimer.stop();

        m_isConnectionInProgress = false;
        m_isConnected = true;
        m_connectionStateText = tr("Connected");
        break;
    }
    case Vpn::ConnectionState::Connecting: {
        {
            const int serverIndex = m_serversModel->getDefaultServerIndex();
            if (serverIndex >= 0) {
                const QVariant containerVar =
                        m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole);
                if (containerVar.isValid()) {
                    const DockerContainer container = qvariant_cast<DockerContainer>(containerVar);
                    const Proto proto = ContainerProps::defaultProtocol(container);
                    if (proto == Proto::Awg) {
                        m_awgStateTimer.start(10000); // 10 seconds
                    } else {
                        m_awgStateTimer.stop();
                    }
                }
            }
        }

        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Reconnecting: {
        m_awgStateTimer.stop();
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Reconnecting...");
        break;
    }
    case Vpn::ConnectionState::Disconnected: {
        m_awgStateTimer.stop();
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        break;
    }
    case Vpn::ConnectionState::Disconnecting: {
        m_awgStateTimer.stop();
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Disconnecting...");
        break;
    }
    case Vpn::ConnectionState::Preparing: {
        m_awgStateTimer.stop();
        m_isConnectionInProgress = true;
        m_connectionStateText = tr("Preparing...");
        break;
    }
    case Vpn::ConnectionState::Error: {
        m_awgStateTimer.stop();
        m_isConnectionInProgress = false;
        m_connectionStateText = tr("Connect");
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    case Vpn::ConnectionState::Unknown: {
        m_awgStateTimer.stop();
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

    const QVariant containerVar =
            m_serversModel->data(serverIndex, ServersModel::Roles::DefaultContainerRole);
    if (!containerVar.isValid()) {
        return;
    }

    const DockerContainer container = qvariant_cast<DockerContainer>(containerVar);
    const Proto proto = ContainerProps::defaultProtocol(container);
    if (proto != Proto::Awg) {
        return;
    }

    const QJsonObject serverConfig = m_serversModel->getServerConfig(serverIndex);
    const QJsonArray containers = serverConfig.value(config_key::containers).toArray();
    bool hasXrayContainer = false;
    for (const QJsonValue &value : containers) {
        const QJsonObject obj = value.toObject();
        const DockerContainer c =
                ContainerProps::containerFromString(obj.value(config_key::container).toString());
        if (c == DockerContainer::Xray) {
            hasXrayContainer = true;
            break;
        }
    }

    if (!hasXrayContainer) {
        qDebug().noquote() << "AWG connect timeout: no XRay container available for server index" << serverIndex;
        return;
    }

    qDebug().noquote() << "AWG connect timeout (10s), switching default container to XRay for server index"
                      << serverIndex << "and reconnecting";

    m_serversModel->setDefaultContainer(serverIndex, static_cast<int>(DockerContainer::Xray));

    if (auto app = amnApp) {
        if (auto core = app->coreController()) {
            if (auto api = core->apiConfigsController()) {
                m_serversModel->setProcessedServerIndex(serverIndex);
                api->setCurrentProtocol(QStringLiteral("vless"));
            }
        }
    }

    closeConnection();

    QTimer::singleShot(500, this, [this]() {
        if (!m_isConnected && !m_isConnectionInProgress) {
            emit prepareConfig();
        }
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
