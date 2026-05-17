#include "connectionUiController.h"

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(MACOS_NE)
    #include <QGuiApplication>
#else
    #include <QApplication>
#endif

#include "amneziaApplication.h"
#include "core/controllers/serversController.h"

ConnectionUiController::ConnectionUiController(ConnectionController* connectionController,
                                                ServersController* serversController,
                                                QObject *parent)
    : QObject(parent),
      m_connectionController(connectionController),
      m_serversController(serversController)
{
    connect(m_connectionController, &ConnectionController::connectionStateChanged, this, &ConnectionUiController::onConnectionStateChanged);
    connect(m_connectionController, &ConnectionController::connectionHealthChanged, this, &ConnectionUiController::onConnectionHealthChanged);

    connect(this, &ConnectionUiController::connectButtonClicked, this, &ConnectionUiController::toggleConnection, Qt::QueuedConnection);

    m_state = Vpn::ConnectionState::Disconnected;
}

void ConnectionUiController::openConnection()
{
    int serverIndex = m_serversController->getDefaultServerIndex();

    ErrorCode errorCode = m_connectionController->openConnection(serverIndex);

    if (errorCode != ErrorCode::NoError) {
        emit connectionErrorOccurred(errorCode);
        return;
    }
}

void ConnectionUiController::closeConnection()
{
    m_connectionController->closeConnection();
}

ErrorCode ConnectionUiController::getLastConnectionError()
{
    return m_connectionController->lastConnectionError();
}

void ConnectionUiController::onConnectionStateChanged(Vpn::ConnectionState state)
{
    m_state = state;
    m_health = m_connectionController->connectionHealth();

    m_isConnected = false;
    updateConnectionStateText();
    switch (state) {
    case Vpn::ConnectionState::Connected: {
        amnApp->networkManager()->clearConnectionCache();

        m_isConnectionInProgress = false;
        m_isConnected = true;
        break;
    }
    case Vpn::ConnectionState::Connecting: {
        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Reconnecting: {
        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Disconnected: {
        m_isConnectionInProgress = false;
        break;
    }
    case Vpn::ConnectionState::Disconnecting: {
        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Preparing: {
        m_isConnectionInProgress = true;
        break;
    }
    case Vpn::ConnectionState::Error: {
        m_isConnectionInProgress = false;
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    case Vpn::ConnectionState::Unknown: {
        m_isConnectionInProgress = false;
        emit connectionErrorOccurred(getLastConnectionError());
        break;
    }
    }
    emit connectionStateChanged();
}

void ConnectionUiController::onConnectionHealthChanged(ConnectionHealth health)
{
    m_health = health;
    updateConnectionStateText();
    emit connectionStateChanged();
}

void ConnectionUiController::onCurrentContainerUpdated()
{
    if (m_isConnected || m_isConnectionInProgress) {
        emit reconnectWithUpdatedContainer(tr("Settings updated successfully, reconnection..."));
        openConnection();
    } else {
        emit reconnectWithUpdatedContainer(tr("Settings updated successfully"));
    }
}

void ConnectionUiController::onTranslationsUpdated()
{
    onConnectionStateChanged(getCurrentConnectionState());
}

Vpn::ConnectionState ConnectionUiController::getCurrentConnectionState()
{
    return m_state;
}

QString ConnectionUiController::connectionStateText() const
{
    return m_connectionStateText;
}

QString ConnectionUiController::connectionDiagnosticText() const
{
    return m_connectionDiagnosticText;
}

bool ConnectionUiController::hasConnectionDiagnostic() const
{
    return m_hasConnectionDiagnostic;
}

bool ConnectionUiController::isConnectionDiagnosticProblem() const
{
    return m_isConnectionDiagnosticProblem;
}

void ConnectionUiController::updateConnectionStateText()
{
    m_connectionStateText = connectionLifecycleText(m_state);
    m_connectionDiagnosticText = connectionHealthText(m_health);
    m_hasConnectionDiagnostic = connectionHealthVisible(m_health);
    m_isConnectionDiagnosticProblem = connectionHealthProblem(m_health);
}

void ConnectionUiController::toggleConnection()
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

bool ConnectionUiController::isConnectionInProgress() const
{
    return m_isConnectionInProgress;
}

bool ConnectionUiController::isConnected() const
{
    return m_isConnected;
}
