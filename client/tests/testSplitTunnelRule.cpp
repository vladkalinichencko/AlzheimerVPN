#include <QTest>

#include "core/utils/splitTunnelRule.h"

using namespace amnezia;

class TestSplitTunnelRule : public QObject
{
    Q_OBJECT

private slots:
    void testExactDomain()
    {
        const auto rule = SplitTunnelRule::fromText("https://Moodle.Innopolis.University/course");
        QVERIFY(rule.isValid());
        QCOMPARE(rule.type(), SplitTunnelRule::Type::ExactHost);
        QCOMPARE(rule.normalizedText(), QString("moodle.innopolis.university"));
        QVERIFY(rule.matchesHost("moodle.innopolis.university"));
        QVERIFY(!rule.matchesHost("x.moodle.innopolis.university"));
    }

    void testWildcardDomain()
    {
        const auto rule = SplitTunnelRule::fromText("*.innodatahub.innopolis.university");
        QVERIFY(rule.isValid());
        QCOMPARE(rule.type(), SplitTunnelRule::Type::WildcardHost);
        QVERIFY(rule.matchesHost("e9f2d330fb4d.innodatahub.innopolis.university"));
        QVERIFY(!rule.matchesHost("innodatahub.innopolis.university"));
    }

    void testWildcardPositions()
    {
        QVERIFY(SplitTunnelRule::fromText("api.*.example.com").matchesHost("api.v1.example.com"));
        QVERIFY(SplitTunnelRule::fromText("*mail.example.com").matchesHost("webmail.example.com"));
        QVERIFY(SplitTunnelRule::fromText("a*b.example.com").matchesHost("alpha-b.example.com"));
        QVERIFY(SplitTunnelRule::fromText("*.*.example.com").matchesHost("a.b.example.com"));
    }

    void testRemovedRuleSyntax()
    {
        QVERIFY(!SplitTunnelRule::fromText("regex:^[a-z]+\\.example\\.com$").isValid());
        QVERIFY(!SplitTunnelRule::fromText("suffix:example.com").isValid());
    }

    void testIpSubnet()
    {
        const auto ip = SplitTunnelRule::fromText("188.130.155.243");
        QVERIFY(ip.isValid());
        QCOMPARE(ip.type(), SplitTunnelRule::Type::IpSubnet);

        const auto subnet = SplitTunnelRule::fromText("104.18.0.0/16");
        QVERIFY(subnet.isValid());
        QCOMPARE(subnet.type(), SplitTunnelRule::Type::IpSubnet);
    }
};

QTEST_MAIN(TestSplitTunnelRule)
#include "testSplitTunnelRule.moc"
