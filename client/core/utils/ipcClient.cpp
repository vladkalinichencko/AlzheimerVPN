#include "ipcClient.h"
#include "ipc.h"
#include <QRemoteObjectNode>
#include <QtNetwork/qlocalsocket.h>

IpcClient::IpcClient(QObject *parent) : QObject(parent)
{
    m_node.connectToNode(QUrl("local:" + amnezia::getIpcServiceUrl()));
    m_interface.reset(m_node.acquire<IpcInterfaceReplica>());
}

IpcClient& IpcClient::Instance()
{
    thread_local IpcClient ipcClient;
    return ipcClient;
}

QSharedPointer<IpcInterfaceReplica> IpcClient::Interface()
{
    QSharedPointer<IpcInterfaceReplica> rep = Instance().m_interface;
    if (rep.isNull()) {
        qCritical() << "IpcClient::Interface(): Failed to acquire replica";
        return nullptr;
    }
    if (!rep->waitForSource(1000)) {
        qCritical() << "IpcClient::Interface(): Failed to initialize replica";
        return nullptr;
    }
    if (!rep->isReplicaValid()) {
        qWarning() << "IpcClient::Interface(): Replica is invalid";
    }
    return rep;
}

QSharedPointer<IpcProcessInterfaceReplica> IpcClient::CreatePrivilegedProcess()
{
    return withInterface([](QSharedPointer<IpcInterfaceReplica> &iface) -> QSharedPointer<IpcProcessInterfaceReplica> {
        qInfo() << "IpcClient::CreatePrivilegedProcess: requesting pid from daemon";
        auto createPrivilegedProcess = iface->createPrivilegedProcess();
        if (!createPrivilegedProcess.waitForFinished()) {
            qCritical() << "IpcClient::CreatePrivilegedProcess: daemon createPrivilegedProcess() timed out";
            return nullptr;
        }

        const int pid = createPrivilegedProcess.returnValue();
        qInfo() << "IpcClient::CreatePrivilegedProcess: daemon returned pid=" << pid;
        if (pid <= 0) {
            qCritical() << "IpcClient::CreatePrivilegedProcess: daemon returned invalid pid" << pid;
            return nullptr;
        }

        const QString url = QString("local:%1").arg(amnezia::getIpcProcessUrl(pid));
        auto* node = new QRemoteObjectNode();
        node->connectToNode(QUrl(url));
        qInfo() << "IpcClient::CreatePrivilegedProcess: connected QRemoteObjectNode to" << url;

        QSharedPointer<IpcProcessInterfaceReplica> rep(
            node->acquire<IpcProcessInterfaceReplica>(),
            [node] (IpcProcessInterfaceReplica *ptr) {
                delete ptr;
                node->deleteLater();
            }
        );
        if (rep.isNull()) {
            qCritical() << "IpcClient::CreatePrivilegedProcess: acquire<IpcProcessInterfaceReplica>() returned null";
            return nullptr;
        }
        if (!rep->waitForSource()) {
            qCritical() << "IpcClient::CreatePrivilegedProcess: replica waitForSource() failed for pid=" << pid;
            return nullptr;
        }
        if (!rep->isReplicaValid()) {
            qCritical() << "IpcClient::CreatePrivilegedProcess: replica isReplicaValid() false for pid=" << pid;
            return nullptr;
        }

        qInfo() << "IpcClient::CreatePrivilegedProcess: replica ready for pid=" << pid;
        return rep;
    },
    []() -> QSharedPointer<IpcProcessInterfaceReplica> {
        qCritical() << "IpcClient::CreatePrivilegedProcess: withInterface onFailure — IPC unavailable";
        return nullptr;
    });
}
