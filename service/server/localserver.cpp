#include "localserver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QSharedPointer>
#include <QString>

#include "ipc.h"
#include "killswitch.h"
#include "logger.h"

#ifdef Q_OS_WIN
    #include "tapcontroller_win.h"
#endif

namespace {
Logger logger("WgDaemonServer");
}

LocalServer::LocalServer(QObject *parent) : QObject(parent),
    m_ipcServer(this)
{
    // Create the server and listen outside of QtRO
    m_server = QSharedPointer<QLocalServer>(new QLocalServer(this));
    m_server->setSocketOptions(QLocalServer::WorldAccessOption);

    // A socket file left by a previous daemon makes listen() fail and leaves clients without a service.
    QLocalServer::removeServer(amnezia::getIpcServiceUrl());

    if (!m_server->listen(amnezia::getIpcServiceUrl())) {
        qDebug() << QString("Unable to start the server: %1.").arg(m_server->errorString());
        return;
    }

    QObject::connect(m_server.data(), &QLocalServer::newConnection, this, [this]() {
        qDebug() << "LocalServer new connection";
        QLocalSocket *socket = m_server->nextPendingConnection();
        ++m_clientCount;
#ifdef Q_OS_MAC
        if (m_clientCount == 1 && KillSwitch::instance()->isStrictKillSwitchEnabled()) {
            KillSwitch::instance()->disableAllTraffic();
        }
#endif
        connect(socket, &QLocalSocket::disconnected, this, [this]() {
            if (--m_clientCount == 0) {
#ifdef Q_OS_MAC
                cleanupClientSession();
#else
                KillSwitch::instance()->setAlwaysBlockedSites({});
#endif
            }
        });
        m_serverNode.addHostSideConnection(socket);

        if (!m_isRemotingEnabled) {
            m_isRemotingEnabled = true;
            m_serverNode.enableRemoting(&m_ipcServer);
        }
    });

    // Init Mozilla Wireguard Daemon
    if (!server.initialize()) {
        logger.error() << "Failed to initialize the server";
        return;
    }

    m_networkWatcher.initialize();
    m_ipcServer.setNetworkWatcher(&m_networkWatcher);
    connect(&m_networkWatcher, &NetworkWatcher::networkChanged, &m_ipcServer, &IpcServer::networkChanged);
    connect(&m_networkWatcher, &NetworkWatcher::wakeup, &m_ipcServer, &IpcServer::wakeup);
    connect(&m_networkWatcher, &NetworkWatcher::physicalNetworkReadyChanged,
            &m_ipcServer, &IpcServer::physicalNetworkReadyChanged);
    KillSwitch::instance()->init();

#ifdef Q_OS_LINUX
    // Signal handling for a proper shutdown.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit,
                     []() { LinuxDaemon::instance()->deactivate(); });
#endif

#ifdef Q_OS_MAC
    connect(qApp, &QCoreApplication::aboutToQuit, this, &LocalServer::cleanupClientSession);
#endif

#ifdef Q_OS_WIN
    // Signal handling for a proper shutdown.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit,
                     []() { WindowsDaemon::instance()->deactivate(); });
#endif
}

#ifdef Q_OS_MAC
void LocalServer::cleanupClientSession()
{
    daemon.deactivate(false);
    m_ipcServer.cleanupClientSession();
    KillSwitch::instance()->endClientSession();
}
#endif

LocalServer::~LocalServer()
{
    qDebug() << "Local server stopped";
}
