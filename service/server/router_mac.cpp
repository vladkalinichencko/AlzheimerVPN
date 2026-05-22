#include "router_mac.h"
#include "helper_route_mac.h"
#include "killswitch.h"

#include <QProcess>
#include <QThread>

#include <core/utils/networkUtilities.h>

RouterMac &RouterMac::Instance()
{
    static RouterMac s;
    return s;
}

RouterMac::RouterMac()
{
    m_dnsUtil = new DnsUtilsMacos(this);
    connect(m_dnsUtil, &DnsUtilsMacos::splitTunnelHostResolved, this,
            [this](const QString &host, const QStringList &ips) {
                if (m_dnsSplitTunnelGateway.isEmpty() || ips.isEmpty()) {
                    return;
                }
                QStringList newIps;
                for (const QString &ip : ips) {
                    if (m_dnsSplitTunnelIps.contains(ip)) {
                        continue;
                    }
                    m_dnsSplitTunnelIps.insert(ip);
                    newIps.append(ip);
                }
                if (newIps.isEmpty()) {
                    return;
                }
                qInfo() << "RouterMac::splitTunnelHostResolved host=" << host << "new_ips=" << newIps;
                routeAddList(m_dnsSplitTunnelGateway, newIps);
                if (m_dnsSplitTunnelKillSwitchEnabled) {
                    KillSwitch::instance()->addAllowedRange(newIps);
                }
            });
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

    QStringList parts = cmd.split(" ");

    int argc = parts.size();
    char **argv = new char*[argc];

    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }

    // TODO refactor
    mainRouteIface(argc, argv);
    m_addedRoutes.append({ipWithSubnet, gw});

    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;
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

    QStringList parts = cmd.split(" ");

    int argc = parts.size();
    char **argv = new char*[argc];

    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }

    mainRouteIface(argc, argv);

    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;
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
    if (m_dnsSplitTunnelGateway != gw || rules.isEmpty()) {
        m_dnsSplitTunnelIps.clear();
    }
    m_dnsSplitTunnelGateway = gw;
    m_dnsSplitTunnelKillSwitchEnabled = killSwitchEnabled;
    m_dnsUtil->configureSplitTunnelRules(rules);
    return true;
}

bool RouterMac::routeAddXray(const QString& ifname, const QString& gateway)
{
    if (ifname.isEmpty() || gateway.isEmpty()) {
        qWarning().noquote() << "routeAddXray: invalid iface/gateway:" << ifname << gateway;
        return false;
    }

    QString cmd = QString("route add -net 0.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    QStringList parts = cmd.split(" ");

    int argc = parts.size();
    char **argv = new char*[argc];
    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }
    mainRouteIface(argc, argv);
    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;

    cmd = QString("route add -net 128.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    parts = cmd.split(" ");

    argc = parts.size();
    argv = new char*[argc];
    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }
    mainRouteIface(argc, argv);
    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;

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
    QStringList parts = cmd.split(" ");

    int argc = parts.size();
    char **argv = new char*[argc];
    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }
    mainRouteIface(argc, argv);
    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;

    if (!gateway.isEmpty()) {
        cmd = QString("route delete -net 128.0.0.0/1 %1 -ifscope %2").arg(gateway).arg(ifname);
    } else {
        cmd = QString("route delete -net 128.0.0.0/1 -ifscope %1").arg(ifname);
    }
    parts = cmd.split(" ");

    argc = parts.size();
    argv = new char*[argc];
    for (int i = 0; i < argc; i++) {
        argv[i] = new char[parts.at(i).toStdString().length() + 1];
        strcpy(argv[i], parts.at(i).toStdString().c_str());
    }
    mainRouteIface(argc, argv);
    for (int i = 0; i < argc; i++) {
        delete [] argv[i];
    }
    delete[] argv;

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
