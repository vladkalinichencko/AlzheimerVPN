#include <QHostAddress>
#include <QHostInfo>
#include <QTest>

#include "core/utils/splitTunnelRoutePlanner.h"

using namespace amnezia;

namespace {
    QHostInfo hostInfo(const QList<QHostAddress> &addresses)
    {
        QHostInfo info;
        info.setAddresses(addresses);
        return info;
    }
}

class TestSplitTunnelRoutePlanner : public QObject
{
    Q_OBJECT

private slots:
    void testResolvedIpUpdatesStoredIp()
    {
        const QStringList ips = SplitTunnelRoutePlanner::resolvedIpv4Routes(
            hostInfo({ QHostAddress("188.130.155.244") }),
            { "188.130.155.243" });
        QCOMPARE(ips, QStringList({ "188.130.155.244" }));
    }

    void testMultipleARecordsProduceMultipleRoutes()
    {
        const QStringList ips = SplitTunnelRoutePlanner::resolvedIpv4Routes(
            hostInfo({ QHostAddress("1.1.1.1"), QHostAddress("2.2.2.2") }));
        QCOMPARE(ips, QStringList({ "1.1.1.1", "2.2.2.2" }));
    }

    void testStaleStoredIpDoesNotBlockNewResolvedIp()
    {
        const QStringList ips = SplitTunnelRoutePlanner::resolvedIpv4Routes(
            hostInfo({ QHostAddress("10.0.0.2") }),
            { "10.0.0.1" });
        QCOMPARE(ips, QStringList({ "10.0.0.2" }));
    }

    void testAlreadyRoutedResolvedIpDoesNotProduceNewRoute()
    {
        const QStringList ips = SplitTunnelRoutePlanner::resolvedIpv4Routes(
            hostInfo({ QHostAddress("10.0.0.1") }),
            { "10.0.0.1" });
        QCOMPARE(ips, QStringList());
    }

    void testPartialFailureRetriesOnlyWhenRetryAvailable()
    {
        const QStringList ips({ "1.1.1.1", "2.2.2.2" });
        QVERIFY(SplitTunnelRoutePlanner::shouldRetryRouteAdd(ips, false, true));
        QVERIFY(!SplitTunnelRoutePlanner::shouldRetryRouteAdd(ips, false, false));
        QVERIFY(!SplitTunnelRoutePlanner::shouldRetryRouteAdd(ips, true, true));
    }

    void testKillSwitchAllowedIpsMatchRouteIps()
    {
        const QStringList ips({ "188.130.155.243", "188.130.155.244" });
        QCOMPARE(SplitTunnelRoutePlanner::killSwitchAllowedIps(RouteMode::VpnAllExceptSites, true, ips), ips);
        QCOMPARE(SplitTunnelRoutePlanner::killSwitchAllowedIps(RouteMode::VpnOnlyForwardSites, true, ips), QStringList());
        QCOMPARE(SplitTunnelRoutePlanner::killSwitchAllowedIps(RouteMode::VpnAllExceptSites, false, ips), QStringList());
    }
};

QTEST_MAIN(TestSplitTunnelRoutePlanner)
#include "testSplitTunnelRoutePlanner.moc"
