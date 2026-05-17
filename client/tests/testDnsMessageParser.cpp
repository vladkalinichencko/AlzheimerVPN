#include <QTest>

#include "core/utils/dnsMessageParser.h"
#include "core/utils/splitTunnelRoutePlanner.h"
#include "core/utils/splitTunnelRule.h"

using namespace amnezia;

namespace {
    void append16(QByteArray &data, quint16 value)
    {
        data.append(char(value >> 8));
        data.append(char(value & 0xff));
    }

    void append32(QByteArray &data, quint32 value)
    {
        data.append(char((value >> 24) & 0xff));
        data.append(char((value >> 16) & 0xff));
        data.append(char((value >> 8) & 0xff));
        data.append(char(value & 0xff));
    }

    void appendName(QByteArray &data, const QString &name)
    {
        for (const QString &label : name.split(".")) {
            const QByteArray bytes = label.toLatin1();
            data.append(char(bytes.size()));
            data.append(bytes);
        }
        data.append(char(0));
    }

    QByteArray query(const QString &host, quint16 type = 1)
    {
        QByteArray data;
        append16(data, 0x1234);
        append16(data, 0x0100);
        append16(data, 1);
        append16(data, 0);
        append16(data, 0);
        append16(data, 0);
        appendName(data, host);
        append16(data, type);
        append16(data, 1);
        return data;
    }

    QByteArray responseWithARecord(const QString &host, const QByteArray &ip)
    {
        QByteArray data = query(host);
        data[2] = char(0x81);
        data[3] = char(0x80);
        data[7] = char(1);
        append16(data, 0xc00c);
        append16(data, 1);
        append16(data, 1);
        append32(data, 60);
        append16(data, 4);
        data.append(ip);
        return data;
    }

    QByteArray responseWithCnameAndA(const QString &host, const QString &canonical, const QByteArray &ip)
    {
        QByteArray data = query(host);
        data[2] = char(0x81);
        data[3] = char(0x80);
        data[7] = char(2);
        append16(data, 0xc00c);
        append16(data, 5);
        append16(data, 1);
        append32(data, 60);
        QByteArray cname;
        appendName(cname, canonical);
        append16(data, cname.size());
        data.append(cname);
        appendName(data, canonical);
        append16(data, 1);
        append16(data, 1);
        append32(data, 60);
        append16(data, 4);
        data.append(ip);
        return data;
    }
}

class TestDnsMessageParser : public QObject
{
    Q_OBJECT

private slots:
    void testQueryHost()
    {
        QCOMPARE(DnsMessageParser::queryHost(query("e9f2d330fb4d.innodatahub.innopolis.university")),
                 QString("e9f2d330fb4d.innodatahub.innopolis.university"));
    }

    void testARecord()
    {
        const QStringList ips = DnsMessageParser::responseIpv4Addresses(
            responseWithARecord("moodle.innopolis.university", QByteArray::fromHex("bc829bf3")));
        QCOMPARE(ips, QStringList({ "188.130.155.243" }));
    }

    void testCnameAndARecord()
    {
        const QStringList ips = DnsMessageParser::responseIpv4Addresses(
            responseWithCnameAndA("mail.example.com", "mail-cdn.example.net", QByteArray::fromHex("01020304")));
        QCOMPARE(ips, QStringList({ "1.2.3.4" }));
    }

    void testAaaaIgnored()
    {
        QByteArray data = query("ipv6.example.com", 28);
        data[2] = char(0x81);
        data[3] = char(0x80);
        QCOMPARE(DnsMessageParser::responseIpv4Addresses(data), QStringList());
    }

    void testMalformed()
    {
        QCOMPARE(DnsMessageParser::queryHost(QByteArray("bad")), QString());
        QCOMPARE(DnsMessageParser::responseIpv4Addresses(QByteArray("bad")), QStringList());
    }

    void testNonMatchingHostnameIgnored()
    {
        const QString host = DnsMessageParser::queryHost(query("static.example.com"));
        const SplitTunnelRule rule = SplitTunnelRule::fromText("*.innodatahub.innopolis.university");
        QVERIFY(rule.isValid());
        QVERIFY(!rule.matchesHost(host));
    }

    void testMatchingWildcardAddsRouteAndKillSwitchAllowList()
    {
        const QString host = DnsMessageParser::queryHost(query("e9f2d330fb4d.innodatahub.innopolis.university"));
        const SplitTunnelRule rule = SplitTunnelRule::fromText("*.innodatahub.innopolis.university");
        QVERIFY(rule.matchesHost(host));

        const QStringList ips = DnsMessageParser::responseIpv4Addresses(
            responseWithARecord(host, QByteArray::fromHex("0a000002")));
        QCOMPARE(ips, QStringList({ "10.0.0.2" }));
        QCOMPARE(SplitTunnelRoutePlanner::killSwitchAllowedIps(RouteMode::VpnAllExceptSites, true, ips), ips);
    }
};

QTEST_MAIN(TestDnsMessageParser)
#include "testDnsMessageParser.moc"
