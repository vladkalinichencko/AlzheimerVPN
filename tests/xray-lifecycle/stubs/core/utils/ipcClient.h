#pragma once
#include <QCoreApplication>
#include <QJsonObject>
#include <QHostAddress>
#include <QProcess>
#include <QSharedPointer>
#include <QTimer>
#include <functional>

template<typename T> struct TestReply {
    T value;
    bool waitForFinished(int = 30000) { QCoreApplication::processEvents(); return true; }
    T returnValue() const { return value; }
};

class IpcProcessInterfaceReplica : public QObject {
    Q_OBJECT
public:
    QByteArray output;
    int starts = 0;
    bool waitForSource() { return true; }
    void setProgram(int) {}
    void setArguments(const QStringList &) {}
    void start() { ++starts; }
    void terminate() {}
    void kill() {}
    void close() {}
    TestReply<QByteArray> readAllStandardError() { return {output}; }
    TestReply<QByteArray> readAllStandardOutput() { return {output}; }
signals:
    void readyReadStandardError();
    void readyReadStandardOutput();
    void finished(int, QProcess::ExitStatus);
};

class IpcInterfaceReplica {
public:
    std::function<void()> duringCreateTun;
    int creates = 0;
    int restores = 0;
    bool isReplicaValid() const { return true; }
    TestReply<bool> xrayStart(const QString &) { return {true}; }
    TestReply<bool> xrayStop() { return {true}; }
    TestReply<bool> deleteTun(const QString &) { return {true}; }
    TestReply<bool> createTun(const QString &, const QString &) {
        ++creates;
        if (duringCreateTun) {
            QTimer::singleShot(0, QCoreApplication::instance(), duringCreateTun);
        }
        return {true};
    }
    TestReply<bool> updateResolvers(const QString &, const QList<QHostAddress> &) { return {true}; }
    TestReply<bool> restoreResolvers() { ++restores; return {true}; }
    TestReply<bool> disableKillSwitch() { return {true}; }
    TestReply<bool> StartRoutingIpv6() { return {true}; }
    TestReply<bool> StopRoutingIpv6() { return {true}; }
    TestReply<bool> enableKillSwitch(const QJsonObject &, int) { return {true}; }
    TestReply<int> routeAddList(const QString &, const QStringList &ips) { return {int(ips.size())}; }
};

class IpcClient {
public:
    inline static QSharedPointer<IpcInterfaceReplica> iface;
    inline static QSharedPointer<IpcProcessInterfaceReplica> process;
    static auto Interface() { return iface; }
    static auto CreatePrivilegedProcess() { return process; }
    template<typename Success, typename Failure>
    static auto withInterface(Success success, Failure) { return success(iface); }
    template<typename Reply, typename Done>
    static void async(QObject *context, Reply reply, Done done) {
        QTimer::singleShot(0, context, [reply, done]() { done(reply.returnValue()); });
    }
};
