#include <QtTest/QtTest>
#include <QSignalSpy>

#include "vpnConnection.h"

class FakePollingProtocol : public VpnProtocol
{
public:
    FakePollingProtocol() : VpnProtocol(QJsonObject()) {}

    ErrorCode start() override { return ErrorCode::NoError; }
    void stop() override {}
    bool requestStatus() override
    {
        ++statusRequests;
        return true;
    }

    int statusRequests = 0;
};

class FakeNonPollingProtocol : public VpnProtocol
{
public:
    FakeNonPollingProtocol() : VpnProtocol(QJsonObject()) {}

    ErrorCode start() override { return ErrorCode::NoError; }
    void stop() override {}
};

class TestableVpnConnection : public VpnConnection
{
public:
    TestableVpnConnection() : VpnConnection(nullptr, nullptr) {}

    using VpnConnection::checkConnectedHealth;
    using VpnConnection::onBytesChanged;
    using VpnConnection::onConnectingTimedOut;
    using VpnConnection::onConnectivityProbeFailed;
    using VpnConnection::onConnectivityProbeSucceeded;
    using VpnConnection::onProtocolError;

    void setProtocol(VpnProtocol *protocol)
    {
        m_vpnProtocol.reset(protocol);
    }

protected:
    void connectConnectivityProbe(QTcpSocket *) override
    {
        // No-op: tests drive onConnectivityProbeSucceeded/Failed directly.
    }
};

class TestConnectionHealth : public QObject
{
    Q_OBJECT

private slots:
    void textForUserVisibleHealthStates()
    {
        QVERIFY(connectionHealthText(ConnectionHealth::Idle).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::PreparingConfig).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::CheckingLocalService).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::StartingProtocol).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::WaitingHandshake).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::CheckingTraffic).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::HandshakeTimeout).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::NoTraffic).isEmpty());
        QVERIFY(!connectionHealthText(ConnectionHealth::LocalServiceUnavailable).isEmpty());
    }

    void lifecycleAndDiagnosticTextStaySeparate()
    {
        QCOMPARE(connectionLifecycleText(Vpn::ConnectionState::Disconnected), QString("Connect"));
        QCOMPARE(connectionLifecycleText(Vpn::ConnectionState::Preparing), QString("Preparing"));
        QCOMPARE(connectionLifecycleText(Vpn::ConnectionState::Reconnecting), QString("Reconnecting"));
        QCOMPARE(connectionLifecycleText(Vpn::ConnectionState::Connected), QString("Connected"));

        QCOMPARE(connectionHealthText(ConnectionHealth::CheckingTraffic), QString("Checking traffic"));
        QCOMPARE(connectionHealthText(ConnectionHealth::NoTraffic), QString("VPN is connected, but traffic is not passing"));

        QCOMPARE(connectionStatusText(Vpn::ConnectionState::Connected, ConnectionHealth::NoTraffic), QString("Connected"));
    }

    void cleanupDiagnosticsDoNotLeakAfterTerminalState()
    {
        TestableVpnConnection connection;

        connection.setConnectionState(Vpn::ConnectionState::Connecting);
        connection.setConnectionDiagnostic(ConnectionHealth::CheckingDns);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::CheckingDns);

        connection.setConnectionState(Vpn::ConnectionState::Disconnected);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::Idle);

        connection.setConnectionDiagnostic(ConnectionHealth::CheckingDns);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::Idle);

        connection.setConnectionState(Vpn::ConnectionState::Connecting);
        connection.setConnectionDiagnostic(ConnectionHealth::DnsFailed);
        connection.setConnectionState(Vpn::ConnectionState::Error);
        connection.setConnectionDiagnostic(ConnectionHealth::CheckingDns);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::DnsFailed);
    }

    void connectingTimeoutBecomesHandshakeTimeout()
    {
        TestableVpnConnection connection;

        connection.setConnectionState(Vpn::ConnectionState::Connecting);
        connection.onConnectingTimedOut();

        QCOMPARE(connection.connectionState(), Vpn::ConnectionState::Error);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::HandshakeTimeout);
        QCOMPARE(connection.lastError(), ErrorCode::VpnHandshakeTimeout);
    }

    void backendFailureBecomesDiagnosticHealth()
    {
        TestableVpnConnection connection;

        connection.onProtocolError(ErrorCode::VpnBackendFailure);

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::LocalServiceUnavailable);
        QCOMPARE(connection.lastError(), ErrorCode::VpnBackendFailure);
    }

    void connectedWithoutTrafficWaitsForActiveProbe()
    {
        TestableVpnConnection connection;
        auto *protocol = new FakePollingProtocol();
        connection.setProtocol(protocol);

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        for (int i = 0; i < 5; ++i) {
            connection.checkConnectedHealth();
        }

        QCOMPARE(protocol->statusRequests, 5);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::CheckingTraffic);
        QCOMPARE(connection.lastError(), ErrorCode::NoError);
    }

    void dnsFailureIsNotOverwrittenByNoTraffic()
    {
        TestableVpnConnection connection;
        auto *protocol = new FakePollingProtocol();
        connection.setProtocol(protocol);

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.setConnectionDiagnostic(ConnectionHealth::DnsFailed);
        for (int i = 0; i < 5; ++i) {
            connection.checkConnectedHealth();
        }

        QCOMPARE(protocol->statusRequests, 0);
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::DnsFailed);
    }

    void receivedBytesMarkConnectionHealthy()
    {
        TestableVpnConnection connection;
        connection.setProtocol(new FakePollingProtocol());

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.onBytesChanged(64, 0);

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::Healthy);
    }

    void nonPollingProtocolWaitsForActiveProbe()
    {
        TestableVpnConnection connection;
        connection.setProtocol(new FakeNonPollingProtocol());

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.checkConnectedHealth();

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::CheckingTraffic);
        QCOMPARE(connection.lastError(), ErrorCode::NoError);
    }

    void activeProbeSuccessMarksConnectionHealthy()
    {
        TestableVpnConnection connection;
        connection.setProtocol(new FakeNonPollingProtocol());

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.onConnectivityProbeSucceeded();

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::Healthy);
        QCOMPARE(connection.lastError(), ErrorCode::NoError);
    }

    void activeProbeFailureReportsNoTrafficWithoutUiReconnect()
    {
        TestableVpnConnection connection;
        connection.setProtocol(new FakeNonPollingProtocol());

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.onConnectivityProbeFailed();

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::NoTraffic);
        QCOMPARE(connection.lastError(), ErrorCode::VpnNoTrafficError);
    }

    void platformPathWithoutProtocolUsesActiveProbe()
    {
        TestableVpnConnection connection;

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.checkConnectedHealth();
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::CheckingTraffic);

        connection.onConnectivityProbeSucceeded();
        QCOMPARE(connection.connectionHealth(), ConnectionHealth::Healthy);
        QCOMPARE(connection.lastError(), ErrorCode::NoError);
    }

    void healthyConnectionDoesNotFailJustBecauseItIsIdle()
    {
        TestableVpnConnection connection;
        connection.setProtocol(new FakePollingProtocol());

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.onBytesChanged(64, 0);
        for (int i = 0; i < 5; ++i) {
            connection.checkConnectedHealth();
        }

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::Healthy);
        QCOMPARE(connection.lastError(), ErrorCode::NoError);
    }

    void healthyConnectionReportsNoTrafficWhenDataPathFails()
    {
        TestableVpnConnection connection;
        connection.setProtocol(new FakePollingProtocol());

        connection.setConnectionState(Vpn::ConnectionState::Connected);
        connection.onBytesChanged(64, 0);
        connection.checkConnectedHealth();
        connection.onConnectivityProbeFailed();

        QCOMPARE(connection.connectionHealth(), ConnectionHealth::NoTraffic);
        QCOMPARE(connection.lastError(), ErrorCode::VpnNoTrafficError);
    }

    void errorStateNeverReportsNoError()
    {
        TestableVpnConnection connection;

        connection.setConnectionState(Vpn::ConnectionState::Error);

        QCOMPARE(connection.lastError(), ErrorCode::UnknownError);
    }
};

QTEST_MAIN(TestConnectionHealth)
#include "testConnectionHealth.moc"
