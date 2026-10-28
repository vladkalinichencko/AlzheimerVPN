#include <QtTest>
#include "core/protocols/xrayProtocol.h"
#include "core/protocols/protocolUtils.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/serialization/serialization.h"

VpnProtocol::VpnProtocol(const QJsonObject &config, QObject *parent)
    : QObject(parent), m_connectionState(Vpn::Unknown), m_rawConfig(config),
      m_timeoutTimer(nullptr), m_lastError(ErrorCode::NoError), m_receivedBytes(0), m_sentBytes(0) {}
bool VpnProtocol::isConnected() const { return m_connectionState == Vpn::Connected; }
bool VpnProtocol::isDisconnected() const { return m_connectionState == Vpn::Disconnected; }
void VpnProtocol::onTimeout() { stop(); }
void VpnProtocol::setBytesChanged(quint64, quint64) {}
void VpnProtocol::setConnectionState(Vpn::ConnectionState state) {
    m_connectionState = state;
    emit connectionStateChanged(state);
}
void VpnProtocol::setLastError(ErrorCode error) { m_lastError = error; }
Vpn::ConnectionState VpnProtocol::connectionState() const { return m_connectionState; }
QString amnezia::ProtocolUtils::key_proto_config_data(Proto) { return "xray"; }
amnezia::serialization::inbounds::InboundCredentials
amnezia::serialization::inbounds::EnsureInboundAuth(QJsonObject &) { return {"test", "test", 1080}; }

class XrayLifecycleTest : public QObject {
    Q_OBJECT
    QSharedPointer<XrayProtocol> protocol;
private slots:
    void init() {
        IpcClient::iface.reset(new IpcInterfaceReplica);
        IpcClient::process.reset(new IpcProcessInterfaceReplica);
        protocol.reset(new XrayProtocol({}));
        QCOMPARE(protocol->start(), ErrorCode::NoError);
        IpcClient::process->output = "[STACK] tun://utun22 <-> socks5://127.0.0.1";
    }
    void cleanup() {
        protocol.reset();
        QCoreApplication::processEvents();
        IpcClient::process.reset();
        IpcClient::iface.reset();
    }
    void stopDuringRouting() {
        IpcClient::iface->duringCreateTun = [this]() { protocol->stop(); };
        emit IpcClient::process->readyReadStandardError();
        QTest::qWait(20);
        QCOMPARE(protocol->connectionState(), Vpn::Disconnected);
    }
    void deleteDuringRouting() {
        QPointer<XrayProtocol> guard(protocol.data());
        IpcClient::iface->duringCreateTun = [this]() { protocol.reset(); };
        emit IpcClient::process->readyReadStandardError();
        QTRY_VERIFY(guard.isNull());
    }
    void setupRunsOnce() {
        emit IpcClient::process->readyReadStandardError();
        emit IpcClient::process->readyReadStandardError();
        QTest::qWait(20);
        QCOMPARE(IpcClient::iface->creates, 1);
        QCOMPARE(protocol->connectionState(), Vpn::Connected);
    }
    void stopIsIdempotent() {
        protocol->stop();
        protocol.reset();
        QCOMPARE(IpcClient::iface->restores, 1);
    }
    void queuedOutputAfterStop() {
        emit IpcClient::process->readyReadStandardError();
        emit IpcClient::process->finished(1, QProcess::NormalExit);
        protocol->stop();
        QTest::qWait(20);
        QCOMPARE(IpcClient::iface->creates, 0);
        QCOMPARE(protocol->connectionState(), Vpn::Disconnected);
    }
    void cancelledRetryDoesNotStart() {
        IpcClient::process->output = "resource busy";
        emit IpcClient::process->finished(1, QProcess::NormalExit);
        QTest::qWait(20);
        protocol->stop();
        QTest::qWait(450);
        QCOMPARE(IpcClient::process->starts, 1);
        QCOMPARE(protocol->connectionState(), Vpn::Disconnected);
    }
};
QTEST_GUILESS_MAIN(XrayLifecycleTest)
#include "main.moc"
