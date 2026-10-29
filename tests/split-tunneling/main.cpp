#include <QtTest>
#include "core/controllers/ipSplitTunnelingController.h"
#include "core/utils/splitTunnelRoutePlanner.h"
#include "ui/models/ipSplitTunnelingModel.h"

using State = IpSplitTunnelingController::CheckState;

class SplitTunnelingTests : public QObject
{
    Q_OBJECT

    static void deliver(IpSplitTunnelingController &controller, const QString &host, QHostInfo info)
    {
        const int id = controller.siteCheck(host).lookupId;
        QHostInfo::abortHostLookup(id);
        info.setLookupId(id);
        QVERIFY(QMetaObject::invokeMethod(&controller, "onHostResolved", Qt::DirectConnection,
                                         Q_ARG(QString, host), Q_ARG(QHostInfo, info)));
    }

private slots:
    void liveResolverReachesSuccessAndFailure()
    {
        if (!qEnvironmentVariableIsSet("SPLIT_TEST_LIVE_DNS")) {
            QSKIP("Set SPLIT_TEST_LIVE_DNS=1 to exercise the system resolver");
        }
        SecureQSettings settings("SplitTunnelingTests", "Memory", nullptr, false);
        SecureAppSettingsRepository repository(&settings);
        IpSplitTunnelingController controller(&repository);
        QVERIFY(controller.addSite("example.com"));
        QVERIFY(controller.addSite("split-check.invalid"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.siteCheck("example.com").state != State::Resolving, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.siteCheck("split-check.invalid").state != State::Resolving, 15000);
        QCOMPARE(controller.siteCheck("example.com").state, State::Resolved);
        QCOMPARE(controller.siteCheck("split-check.invalid").state, State::Failed);
        QVERIFY(!controller.siteCheck("split-check.invalid").error.isEmpty());
    }

    void lookupFailureAndRetryReachTheRow()
    {
        SecureQSettings settings("SplitTunnelingTests", "Memory", nullptr, false);
        SecureAppSettingsRepository repository(&settings);
        IpSplitTunnelingController controller(&repository);
        IpSplitTunnelingModel model;
        model.setController(&controller);
        connect(&repository, &SecureAppSettingsRepository::sitesChanged, &model, [&] {
            model.updateModel(controller.getCurrentSites());
        });
        QVERIFY(controller.addSite("missing.invalid"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(controller.siteCheck("missing.invalid").state, State::Resolving);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QHostInfo failure;
        failure.setError(QHostInfo::HostNotFound);
        failure.setErrorString("Host not found");
        deliver(controller, "missing.invalid", failure);
        QVERIFY(model.data(model.index(0), IpSplitTunnelingModel::CheckFailedRole).toBool());
        QCOMPARE(controller.siteCheck("missing.invalid").error, "Host not found");
        QVERIFY(!changed.isEmpty());
        controller.checkSite("missing.invalid");
        QVERIFY(!model.data(model.index(0), IpSplitTunnelingModel::CheckSucceededRole).toBool());
        QVERIFY(!model.data(model.index(0), IpSplitTunnelingModel::CheckFailedRole).toBool());
        QHostInfo success;
        success.setAddresses({QHostAddress("192.0.2.1"), QHostAddress("192.0.2.2"), QHostAddress("2001:db8::1")});
        deliver(controller, "missing.invalid", success);
        QCOMPARE(controller.siteCheck("missing.invalid").state, State::Resolved);
        QCOMPARE(controller.getCurrentSites().first().second, QStringList({"192.0.2.1", "192.0.2.2"}));
        QVERIFY(!model.data(model.index(0), IpSplitTunnelingModel::CheckFailedRole).toBool());
        QVERIFY(model.data(model.index(0), IpSplitTunnelingModel::CheckSucceededRole).toBool());
        const auto exported = QJsonDocument::fromJson(controller.exportSitesToJson()).array().first().toObject();
        QCOMPARE(exported.value("ips").toArray().size(), 2);
        QCOMPARE(exported.keys(), QStringList({"hostname", "ip", "ips"}));
    }

    void failedRefreshKeepsSavedAddresses()
    {
        SecureQSettings settings("SplitTunnelingTests", "Memory", nullptr, false);
        SecureAppSettingsRepository repository(&settings);
        IpSplitTunnelingController controller(&repository);
        controller.addSites({{"saved.invalid", {"192.0.2.10"}}}, false);
        IpSplitTunnelingModel model;
        model.setController(&controller);
        model.updateModel(controller.getCurrentSites());
        QVERIFY(model.data(model.index(0), IpSplitTunnelingModel::CheckSucceededRole).toBool());
        QVERIFY(!model.data(model.index(0), IpSplitTunnelingModel::CheckFailedRole).toBool());
        QCOMPARE(controller.siteCheck("saved.invalid").state, State::Unchecked);
        controller.checkSite("saved.invalid");
        QHostInfo noIpv4;
        noIpv4.setAddresses({QHostAddress("2001:db8::1")});
        deliver(controller, "saved.invalid", noIpv4);
        QCOMPARE(controller.siteCheck("saved.invalid").state, State::Failed);
        QVERIFY(!controller.siteCheck("saved.invalid").error.isEmpty());
        QVERIFY(!model.data(model.index(0), IpSplitTunnelingModel::CheckSucceededRole).toBool());
        QVERIFY(model.data(model.index(0), IpSplitTunnelingModel::CheckFailedRole).toBool());
        QCOMPARE(SecureAppSettingsRepository::siteIpList(repository.vpnSites(controller.getRouteMode()).value("saved.invalid")),
                 QStringList({"192.0.2.10"}));
    }

    void removedAndReplacedRulesIgnoreLateAnswers()
    {
        SecureQSettings settings("SplitTunnelingTests", "Memory", nullptr, false);
        SecureAppSettingsRepository repository(&settings);
        IpSplitTunnelingController controller(&repository);
        QVERIFY(controller.addSite("late.invalid"));
        QHostInfo stale;
        stale.setLookupId(controller.siteCheck("late.invalid").lookupId);
        stale.setAddresses({QHostAddress("192.0.2.99")});
        QVERIFY(controller.removeSite("late.invalid"));
        QVERIFY(controller.addSite("late.invalid"));
        QVERIFY(QMetaObject::invokeMethod(&controller, "onHostResolved", Qt::DirectConnection,
                                         Q_ARG(QString, QString("late.invalid")), Q_ARG(QHostInfo, stale)));
        QVERIFY(controller.getCurrentSites().first().second.isEmpty());
        controller.setRouteMode(RouteMode::VpnAllExceptSites);
        QVERIFY(QMetaObject::invokeMethod(&controller, "onHostResolved", Qt::DirectConnection,
                                         Q_ARG(QString, QString("late.invalid")), Q_ARG(QHostInfo, stale)));
        QVERIFY(repository.vpnSites(RouteMode::VpnAllExceptSites).isEmpty());
        controller.addSites({{"replacement.invalid", {"192.0.2.5"}}}, true);
        QVERIFY(QMetaObject::invokeMethod(&controller, "onHostResolved", Qt::DirectConnection,
                                         Q_ARG(QString, QString("late.invalid")), Q_ARG(QHostInfo, stale)));
        QCOMPARE(controller.getCurrentSites().size(), 1);
        QCOMPARE(controller.getCurrentSites().first().first, "replacement.invalid");
    }

    void importChecksEmptyHostsButNotMasksOrIpRules()
    {
        SecureQSettings settings("SplitTunnelingTests", "Memory", nullptr, false);
        SecureAppSettingsRepository repository(&settings);
        IpSplitTunnelingController controller(&repository);
        QString error;
        QVERIFY(controller.importSitesFromJson(R"([
            {"hostname":"empty.invalid","ips":[]},
            {"hostname":"saved.invalid","ips":["192.0.2.5"]},
            {"hostname":"*.example.com","ips":[]},
            {"hostname":"192.0.2.7","ips":[]}
        ])", true, error));
        QCOMPARE(controller.siteCheck("empty.invalid").state, State::Resolving);
        QCOMPARE(controller.siteCheck("saved.invalid").state, State::Unchecked);
        QCOMPARE(controller.siteCheck("*.example.com").state, State::Dynamic);
        QCOMPARE(controller.siteCheck("192.0.2.7").state, State::Address);
        IpSplitTunnelingModel model;
        model.setController(&controller);
        model.updateModel(controller.getCurrentSites());
        for (int row = 0; row < model.rowCount(); ++row) {
            const auto index = model.index(row);
            const auto host = model.data(index, IpSplitTunnelingModel::UrlRole).toString();
            const bool hasAddress = host == "saved.invalid" || host == "192.0.2.7";
            QCOMPARE(model.data(index, IpSplitTunnelingModel::CheckSucceededRole).toBool(), hasAddress);
            QVERIFY(!model.data(index, IpSplitTunnelingModel::CheckFailedRole).toBool());
        }
        model.updateModel({{"unchecked.invalid", {}}});
        QVERIFY(model.data(model.index(0), IpSplitTunnelingModel::CheckFailedRole).toBool());
        controller.checkSite("*.example.com");
        controller.checkSite("192.0.2.7");
        QCOMPARE(controller.siteCheck("*.example.com").lookupId, -1);
        QCOMPARE(controller.siteCheck("192.0.2.7").lookupId, -1);
        controller.removeSites();
        QVERIFY(controller.getCurrentSites().isEmpty());
        QCOMPARE(controller.siteCheck("empty.invalid").state, State::Unchecked);
    }

    void cachedSiteIpsCombineCacheAndSubnets()
    {
        const QVariantMap sites {
            {"video.example", QStringList {"203.0.113.5", "203.0.113.6"}},
            {"*.cdn.example", QStringList {"203.0.113.9"}},
            {"198.51.100.4", QStringList {}},
            {"203.0.113.5", QStringList {}},
            {"not a host!", QStringList {"192.0.2.99"}},
        };
        QStringList ips = amnezia::SplitTunnelRoutePlanner::cachedSiteIps(sites);
        ips.sort();
        QCOMPARE(ips, (QStringList {"198.51.100.4", "203.0.113.5", "203.0.113.6", "203.0.113.9"}));
        QVERIFY(amnezia::SplitTunnelRoutePlanner::cachedSiteIps({}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SplitTunnelingTests)
#include "main.moc"
