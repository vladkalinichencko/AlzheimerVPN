#ifndef IPCCLIENT_H
#define IPCCLIENT_H

#include <QLocalSocket>
#include <QObject>
#include <QRemoteObjectPendingCall>

#include "rep_ipc_interface_replica.h"
#include "rep_ipc_process_interface_replica.h"

class IpcClient : public QObject
{
    Q_OBJECT
public:
    explicit IpcClient(QObject *parent = nullptr);

    static IpcClient& Instance();

    static QSharedPointer<IpcInterfaceReplica> Interface();
    static QSharedPointer<IpcProcessInterfaceReplica> CreatePrivilegedProcess();

    template <typename Func>
    static auto withInterface(Func func)
    {
        QSharedPointer<IpcInterfaceReplica> iface = Instance().m_interface;
        using ReturnType = decltype(func(std::declval<QSharedPointer<IpcInterfaceReplica>>()));

        if (iface.isNull() || !iface->waitForSource(1000) || !iface->isReplicaValid()) {
            qWarning() << "IpcClient::withInterface(): Service is not running";

            if constexpr (std::is_void_v<ReturnType>)
                return;
            else
                return ReturnType{};
        }

        return func(iface);
    }

    template <typename OnSuccess, typename OnFailure>
    static auto withInterface(OnSuccess onSuccess, OnFailure onFailure)
    {
        QSharedPointer<IpcInterfaceReplica> iface = Instance().m_interface;
        if (iface.isNull() || !iface->waitForSource(1000) || !iface->isReplicaValid()) {
            return onFailure();
        }

        return onSuccess(iface);
    }

    // Async wrapper for typed QRO replies. Use this instead of reply.waitForFinished()
    // anywhere the call can re-enter (per-site DNS callbacks, signal handlers that
    // can refire while still waiting). waitForFinished spins a nested Qt event
    // loop and will dispatch the next pending re-entry on the same stack frame —
    // with enough re-entries you hit the stack guard and SIGBUS.
    //
    // Usage:
    //   IpcClient::async(this, iface->flushDns(), [](bool ok){ ... });
    //   IpcClient::async(this, iface->routeAddList(gw, ips),
    //                    [count = ips.count()](int n){ return n == count; });
    //
    // `context` owns the watcher (auto-deletes if context dies). `onDone` runs
    // on context's thread when the reply arrives.
    template <typename Reply, typename OnDone>
    static void async(QObject *context, Reply reply, OnDone onDone)
    {
        auto *watcher = new QRemoteObjectPendingCallWatcher(reply, context);
        QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished, context,
            [reply, onDone](QRemoteObjectPendingCallWatcher *w) mutable {
                w->deleteLater();
                onDone(reply.returnValue());
            });
    }
signals:

private:
    QRemoteObjectNode m_node;
    QSharedPointer<IpcInterfaceReplica> m_interface;
};

#endif // IPCCLIENT_H
