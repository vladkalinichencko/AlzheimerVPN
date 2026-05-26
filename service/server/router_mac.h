#ifndef ROUTERMAC_H
#define ROUTERMAC_H

#include <QTimer>
#include <QString>
#include <QStringList>
#include <QSettings>
#include <QHash>
#include <QDebug>
#include <QDateTime>
#include <QObject>
#include <QList>
#include <QSet>

#include "../client/platforms/macos/daemon/dnsutilsmacos.h"

/**
 * @brief The Router class - General class for handling ip routing
 */
class RouterMac : public QObject
{
    Q_OBJECT
public:    
    static RouterMac& Instance();

    struct Route {
        QString dst;
        QString gw;
    };

    bool routeAdd(const QString &ip, const QString &gw);
    int routeAddList(const QString &gw, const QStringList &ips);
    bool clearSavedRoutes();
    bool routeDelete(const QString &ip, const QString &gw);
    bool routeDeleteList(const QString &gw, const QStringList &ips);
    bool flushDns();
    bool createTun(const QString &dev, const QString &subnet);
    bool deleteTun(const QString &dev);
    bool updateResolvers(const QString& ifname, const QList<QHostAddress>& resolvers);
    bool restoreResolvers();
    bool configureDnsSplitTunnel(const QStringList &rules, const QString &gw, bool killSwitchEnabled);
    bool routeAddXray(const QString& ifname, const QString& gateway);
    bool routeDeleteXray(const QString& ifname, const QString& gateway);
    
public slots:

private:
    RouterMac();
    RouterMac(RouterMac const &) = delete;
    RouterMac& operator= (RouterMac const&) = delete;

    bool routeAddTransient(const QString &ipWithSubnet, const QString &gw);
    void updateDnsSplitTunnelHostLeases(const QString &host, const QList<amnezia::DnsIpv4Answer> &answers);
    void seedDnsSplitTunnelLeasesFromCache(const QStringList &rules);
    bool dnsSplitTunnelHostMatchesRules(const QString &host, const QStringList &rules) const;
    void expireDnsSplitTunnelLeases();
    void clearDnsSplitTunnelLeases();
    QSet<QString> activeDnsSplitTunnelIps(const QDateTime &now) const;
    void syncDnsSplitTunnelIps(const QSet<QString> &before, const QDateTime &now);
    void scheduleDnsSplitTunnelLeaseTimer(const QDateTime &now);

    QList<Route> m_addedRoutes;
    DnsUtilsMacos *m_dnsUtil;
    QString m_dnsSplitTunnelGateway;
    bool m_dnsSplitTunnelKillSwitchEnabled = false;
    QSet<QString> m_dnsSplitTunnelIps;
    QStringList m_dnsSplitTunnelRulesKey;
    QHash<QString, QHash<QString, QDateTime>> m_dnsSplitTunnelHostLeases;
    QHash<QString, QList<amnezia::DnsIpv4Answer>> m_dnsSplitTunnelHostCache;
    QTimer m_dnsSplitTunnelLeaseTimer;
};

#endif // ROUTERMAC_H
