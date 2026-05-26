#include "router_mac.h"
#include "helper_route_mac.h"
#include "killswitch.h"

#include <algorithm>

#include <QProcess>
#include <QThread>

#include <cerrno>

#include <core/utils/networkUtilities.h>
#include <core/utils/splitTunnelRule.h>

namespace {
int runRouteCommand(const QString &cmd)
{
    const QStringList parts = cmd.split(" ");

    int argc = parts.size();
    char **argv = new char*[argc];

    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }

    const int result = mainRouteIface(argc, argv);

    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;
    return result;
}
}

RouterMac &RouterMac::Instance()
{
    static RouterMac s;
    return s;
}

RouterMac::RouterMac()
{
    m_dnsUtil = new DnsUtilsMacos(this);
    m_dnsSplitTunnelLeaseTimer.setSingleShot(true);
    connect(&m_dnsSplitTunnelLeaseTimer, &QTimer::timeout,
            this, &RouterMac::expireDnsSplitTunnelLeases);
    connect(m_dnsUtil, &DnsUtilsMacos::splitTunnelHostResolvedWithTtl,
            this, &RouterMac::updateDnsSplitTunnelHostLeases);
}

bool RouterMac::routeAdd(const QString &ipWithSubnet, const QString &gw)
{
    QString ip = NetworkUtilities::ipAddressFromIpWithSubnet(ipWithSubnet);
    QString mask = NetworkUtilities::netMaskFromIpWithSubnet(ipWithSubnet);

#ifdef MZ_DEBUG
    qDebug().noquote() << "RouterMac::routeAdd: " << ipWithSubnet << gw;
#endif

    if (!NetworkUtilities::checkIPv4Format(ip) || !NetworkUtilities::checkIPv4Format(gw)) {
        qCritical().noquote() << "Critical, trying to add invalid route: " << ip << gw;
        return false;
    }

    QString cmd;
    if (mask == "255.255.255.255") {
        cmd = QString("route add -host %1 %2").arg(ip).arg(gw);
    }
    else {
        cmd = QString("route add -net %1 %2 %3").arg(ip).arg(gw).arg(mask);
    }

    const int result = runRouteCommand(cmd);
    if (result != 0 && result != EEXIST) {
        qWarning().noquote() << "RouterMac::routeAdd failed result=" << result << "cmd=" << cmd;
        return false;
    }
    m_addedRoutes.append({ipWithSubnet, gw});
    return true;
}

int RouterMac::routeAddList(const QString &gw, const QStringList &ips)
{
    int cnt = 0;
    for (const QString &ip: ips) {
        if (routeAdd(ip, gw)) cnt++;
    }
    return cnt;
}

bool RouterMac::routeAddTransient(const QString &ipWithSubnet, const QString &gw)
{
    QString ip = NetworkUtilities::ipAddressFromIpWithSubnet(ipWithSubnet);
    QString mask = NetworkUtilities::netMaskFromIpWithSubnet(ipWithSubnet);

    if (!NetworkUtilities::checkIPv4Format(ip) || !NetworkUtilities::checkIPv4Format(gw)) {
        qCritical().noquote() << "Critical, trying to add invalid transient route: " << ip << gw;
        return false;
    }

    QString cmd;
    if (mask == "255.255.255.255") {
        cmd = QString("route add -host %1 %2").arg(ip).arg(gw);
    } else {
        cmd = QString("route add -net %1 %2 %3").arg(ip).arg(gw).arg(mask);
    }

    const int result = runRouteCommand(cmd);
    if (result == 0) {
        return true;
    }
    if (result == EEXIST) {
        qInfo().noquote() << "RouterMac::routeAddTransient already exists cmd=" << cmd;
    } else {
        qWarning().noquote() << "RouterMac::routeAddTransient failed result=" << result << "cmd=" << cmd;
    }
    return false;
}

bool RouterMac::clearSavedRoutes()
{
    int cnt = 0;
    for (const Route &r: m_addedRoutes) {
        if (routeDelete(r.dst, r.gw)) cnt++;
    }
    bool ret = (cnt == m_addedRoutes.count());
    m_addedRoutes.clear();
    return ret;
}

bool RouterMac::routeDelete(const QString &ipWithSubnet, const QString &gw)
{
    QString ip = NetworkUtilities::ipAddressFromIpWithSubnet(ipWithSubnet);
    QString mask = NetworkUtilities::netMaskFromIpWithSubnet(ipWithSubnet);

#ifdef MZ_DEBUG
    qDebug().noquote() << "RouterMac::routeDelete: " << ipWithSubnet << gw;
#endif

    if (!NetworkUtilities::checkIPv4Format(ip) || !NetworkUtilities::checkIPv4Format(gw)) {
        qCritical().noquote() << "Critical, trying to remove invalid route: " << ip << gw;
        return false;
    }

    if (ipWithSubnet == "0.0.0.0/0") {
        qDebug().noquote() << "Warning, trying to remove default route, skipping: " << ip << gw;
        return true;
    }

    QString cmd;
    if (mask == "255.255.255.255") {
        cmd = QString("route delete -host %1 %2").arg(ip).arg(gw);
    }
    else {
        cmd = QString("route delete -net %1 %2 %3").arg(ip).arg(gw).arg(mask);
    }

    const int result = runRouteCommand(cmd);
    if (result != 0 && result != ESRCH) {
        qWarning().noquote() << "RouterMac::routeDelete failed result=" << result << "cmd=" << cmd;
        return false;
    }
    return true;
}

bool RouterMac::routeDeleteList(const QString &gw, const QStringList &ips)
{
    int cnt = 0;
    for (const QString &ip: ips) {
        if (routeDelete(ip, gw)) cnt++;
    }
    return cnt;
}

bool RouterMac::createTun(const QString &dev, const QString &subnet) {
    qDebug().noquote() << "createTun start";

    QProcess process;
    QStringList commands;

    commands << "ifconfig" << dev << "inet" << subnet << subnet << "up";
    process.start("sudo", commands);
    if (!process.waitForStarted(1000))
    {
        qDebug().noquote() << "Could not start activate tun device!\n";
        return false;
    }
    else if (!process.waitForFinished(2000))
    {
        qDebug().noquote() << "Could not activate tun device!\n";
        return false;
    }
    commands.clear();

    return true;
}

bool RouterMac::updateResolvers(const QString& ifname, const QList<QHostAddress>& resolvers)
{
    return m_dnsUtil->updateResolvers(ifname, resolvers);
}

bool RouterMac::restoreResolvers() {
    return m_dnsUtil->restoreResolvers();
}

bool RouterMac::configureDnsSplitTunnel(const QStringList &rules, const QString &gw, bool killSwitchEnabled)
{
    QStringList rulesKey = rules;
    std::sort(rulesKey.begin(), rulesKey.end());
    if (m_dnsSplitTunnelGateway != gw || m_dnsSplitTunnelRulesKey != rulesKey || rules.isEmpty()) {
        clearDnsSplitTunnelLeases();
    }
    m_dnsSplitTunnelGateway = gw;
    m_dnsSplitTunnelKillSwitchEnabled = killSwitchEnabled;
    m_dnsSplitTunnelRulesKey = rulesKey;
    qInfo() << "RouterMac::configureDnsSplitTunnel rules=" << rules.count()
            << "gateway=" << gw << "killswitch=" << killSwitchEnabled;
    m_dnsUtil->configureSplitTunnelRules(rules);
    seedDnsSplitTunnelLeasesFromCache(rules);
    return true;
}

void RouterMac::updateDnsSplitTunnelHostLeases(const QString &host, const QList<amnezia::DnsIpv4Answer> &answers)
{
    if (m_dnsSplitTunnelGateway.isEmpty() || answers.isEmpty()) {
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QSet<QString> before = m_dnsSplitTunnelIps;

    QHash<QString, QDateTime> leases;
    for (const amnezia::DnsIpv4Answer &answer : answers) {
        if (!NetworkUtilities::checkIPv4Format(answer.address)) {
            continue;
        }
        const int ttl = qBound(1, answer.ttlSeconds, 86400);
        leases.insert(answer.address, now.addSecs(ttl));
    }

    if (leases.isEmpty()) {
        m_dnsSplitTunnelHostLeases.remove(host);
        m_dnsSplitTunnelHostCache.remove(host);
    } else {
        m_dnsSplitTunnelHostLeases.insert(host, leases);
        m_dnsSplitTunnelHostCache.insert(host, answers);
    }

    syncDnsSplitTunnelIps(before, now);
    scheduleDnsSplitTunnelLeaseTimer(now);
}

void RouterMac::seedDnsSplitTunnelLeasesFromCache(const QStringList &rules)
{
    if (m_dnsSplitTunnelGateway.isEmpty() || rules.isEmpty() || m_dnsSplitTunnelHostCache.isEmpty()) {
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QSet<QString> before = m_dnsSplitTunnelIps;
    int seededHosts = 0;
    for (auto cacheIt = m_dnsSplitTunnelHostCache.constBegin(); cacheIt != m_dnsSplitTunnelHostCache.constEnd(); ++cacheIt) {
        if (!dnsSplitTunnelHostMatchesRules(cacheIt.key(), rules)) {
            continue;
        }

        QHash<QString, QDateTime> leases;
        for (const amnezia::DnsIpv4Answer &answer : cacheIt.value()) {
            if (!NetworkUtilities::checkIPv4Format(answer.address)) {
                continue;
            }
            leases.insert(answer.address, now.addSecs(qBound(1, answer.ttlSeconds, 86400)));
        }
        if (!leases.isEmpty()) {
            m_dnsSplitTunnelHostLeases.insert(cacheIt.key(), leases);
            ++seededHosts;
        }
    }

    if (seededHosts > 0) {
        qInfo() << "RouterMac::splitTunnelLease seed_from_cache hosts=" << seededHosts;
        syncDnsSplitTunnelIps(before, now);
        scheduleDnsSplitTunnelLeaseTimer(now);
    }
}

bool RouterMac::dnsSplitTunnelHostMatchesRules(const QString &host, const QStringList &rules) const
{
    for (const QString &ruleText : rules) {
        const amnezia::SplitTunnelRule rule = amnezia::SplitTunnelRule::fromText(ruleText);
        if (rule.isValid() && rule.matchesHost(host)) {
            return true;
        }
    }
    return false;
}

void RouterMac::expireDnsSplitTunnelLeases()
{
    m_dnsSplitTunnelLeaseTimer.stop();
    qInfo() << "RouterMac::splitTunnelLease ttl_due keeping learned routes until DNS refresh or disconnect";
}

void RouterMac::clearDnsSplitTunnelLeases()
{
    m_dnsSplitTunnelLeaseTimer.stop();
    QStringList ips;
    for (const QString &ip : std::as_const(m_dnsSplitTunnelIps)) {
        ips.append(ip);
    }
    if (!ips.isEmpty() && !m_dnsSplitTunnelGateway.isEmpty()) {
        routeDeleteList(m_dnsSplitTunnelGateway, ips);
        if (m_dnsSplitTunnelKillSwitchEnabled) {
            KillSwitch::instance()->removeAllowedRange(ips);
        }
    }
    m_dnsSplitTunnelIps.clear();
    m_dnsSplitTunnelHostLeases.clear();
}

QSet<QString> RouterMac::activeDnsSplitTunnelIps(const QDateTime &now) const
{
    QSet<QString> result;
    for (const auto &leases : m_dnsSplitTunnelHostLeases) {
        for (auto leaseIt = leases.constBegin(); leaseIt != leases.constEnd(); ++leaseIt) {
            if (leaseIt.value() > now) {
                result.insert(leaseIt.key());
            }
        }
    }
    return result;
}

QSet<QString> RouterMac::savedRouteIpsForGateway(const QString &gw) const
{
    QSet<QString> result;
    for (const Route &route : m_addedRoutes) {
        if (route.gw == gw) {
            result.insert(NetworkUtilities::ipAddressFromIpWithSubnet(route.dst));
        }
    }
    return result;
}

void RouterMac::syncDnsSplitTunnelIps(const QSet<QString> &before, const QDateTime &now)
{
    QSet<QString> desired = activeDnsSplitTunnelIps(now);
    desired.subtract(savedRouteIpsForGateway(m_dnsSplitTunnelGateway));

    QStringList added;
    QStringList removed;

    for (const QString &ip : desired) {
        if (!before.contains(ip) && routeAddTransient(ip, m_dnsSplitTunnelGateway)) {
            m_dnsSplitTunnelIps.insert(ip);
            added.append(ip);
        }
    }

    for (const QString &ip : before) {
        if (!desired.contains(ip) && routeDelete(ip, m_dnsSplitTunnelGateway)) {
            m_dnsSplitTunnelIps.remove(ip);
            removed.append(ip);
        }
    }

    if (!added.isEmpty()) {
        qInfo() << "RouterMac::splitTunnelLease add gateway=" << m_dnsSplitTunnelGateway
                << "ips=" << added;
        if (m_dnsSplitTunnelKillSwitchEnabled) {
            KillSwitch::instance()->addAllowedRange(added);
        }
    }
    if (!removed.isEmpty()) {
        qInfo() << "RouterMac::splitTunnelLease remove gateway=" << m_dnsSplitTunnelGateway
                << "ips=" << removed;
        if (m_dnsSplitTunnelKillSwitchEnabled) {
            KillSwitch::instance()->removeAllowedRange(removed);
        }
    }
}

void RouterMac::scheduleDnsSplitTunnelLeaseTimer(const QDateTime &now)
{
    QDateTime nextExpiry;
    for (const auto &leases : m_dnsSplitTunnelHostLeases) {
        for (const QDateTime &expiry : leases) {
            if (expiry > now && (!nextExpiry.isValid() || expiry < nextExpiry)) {
                nextExpiry = expiry;
            }
        }
    }

    if (!nextExpiry.isValid()) {
        m_dnsSplitTunnelLeaseTimer.stop();
        return;
    }

    m_dnsSplitTunnelLeaseTimer.start(int(qMax<qint64>(1, now.msecsTo(nextExpiry))));
}

bool RouterMac::routeAddXray(const QString& ifname, const QString& gateway)
{
    if (ifname.isEmpty() || gateway.isEmpty()) {
        qWarning().noquote() << "routeAddXray: invalid iface/gateway:" << ifname << gateway;
        return false;
    }

    const QString firstHalf = QString("route add -net 0.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    const QString secondHalf = QString("route add -net 128.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    const int firstResult = runRouteCommand(firstHalf);
    const int secondResult = runRouteCommand(secondHalf);
    const bool firstOk = firstResult == 0 || firstResult == EEXIST;
    const bool secondOk = secondResult == 0 || secondResult == EEXIST;
    if (!firstOk || !secondOk) {
        qWarning().noquote() << "routeAddXray failed ifname=" << ifname
                             << "gateway=" << gateway
                             << "first_result=" << firstResult
                             << "second_result=" << secondResult;
        return false;
    }

    qDebug().noquote() << "Installed xray routes via" << gateway << "on" << ifname;
    return true;
}

bool RouterMac::routeDeleteXray(const QString& ifname, const QString& gateway)
{
    if (ifname.isEmpty()) {
        return false;
    }

    QString cmd;
    if (!gateway.isEmpty()) {
        cmd = QString("route delete -net 0.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    } else {
        cmd = QString("route delete -net 0.0.0.0/1 -ifscope %1").arg(ifname);
    }
    const int firstResult = runRouteCommand(cmd);

    if (!gateway.isEmpty()) {
        cmd = QString("route delete -net 128.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    } else {
        cmd = QString("route delete -net 128.0.0.0/1 -ifscope %1").arg(ifname);
    }
    const int secondResult = runRouteCommand(cmd);
    if ((firstResult != 0 && firstResult != ESRCH) ||
        (secondResult != 0 && secondResult != ESRCH)) {
        qWarning().noquote() << "routeDeleteXray failed ifname=" << ifname
                             << "first_result=" << firstResult
                             << "second_result=" << secondResult;
        return false;
    }

    qDebug().noquote() << "Removed xray routes on" << ifname;
    return true;
}

bool RouterMac::deleteTun(const QString &dev)
{
    qDebug().noquote() << "deleteTun start" << dev;
    if (dev.isEmpty()) {
        return true;
    }

    auto tunExists = [&dev]() {
        QProcess ifconfig;
        ifconfig.start("/sbin/ifconfig", { dev });
        ifconfig.waitForFinished(500);
        return ifconfig.exitStatus() == QProcess::NormalExit && ifconfig.exitCode() == 0;
    };

    // On macOS the utun device is owned by the process that opened it (tun2socks).
    // The kernel only frees the device when that process exits. If a previous
    // tun2socks is still alive (e.g. the GUI crashed/restarted without a clean
    // teardown), the next xray connect spawns a new tun2socks that fails with
    // "create tun: resource busy". Kill any lingering tun2socks bound to this dev.
    QProcess pkill;
    pkill.setProcessChannelMode(QProcess::MergedChannels);
    pkill.start("/usr/bin/pkill", { "-f", QStringLiteral("tun2socks.*tun://%1").arg(dev) });
    pkill.waitForFinished(2000);
    qDebug().noquote() << "deleteTun pkill exit=" << pkill.exitCode()
                       << "out=" << QString::fromUtf8(pkill.readAll()).trimmed();

    for (int i = 0; i < 10; ++i) {
        if (!tunExists()) {
            return true;
        }
        QThread::msleep(200);
    }

    qWarning().noquote() << "deleteTun failed: device still exists" << dev;
    return false;
}

bool RouterMac::flushDns()
{
    // sudo killall -HUP mDNSResponder
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);

    p.start("killall", QStringList() << "-HUP" << "mDNSResponder");
    p.waitForFinished();
    
    qDebug().noquote() << "OUTPUT killall -HUP mDNSResponder: " + p.readAll();
    return true;
}
