#ifndef ROUTERMAC_H
#define ROUTERMAC_H

#include <QTimer>
#include <QString>
#include <QStringList>
#include <QSettings>
#include <QHash>
#include <QDebug>
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

    QList<Route> m_addedRoutes;
    DnsUtilsMacos *m_dnsUtil;
    QString m_dnsSplitTunnelGateway;
    bool m_dnsSplitTunnelKillSwitchEnabled = false;
    QSet<QString> m_dnsSplitTunnelIps;
};

#endif // ROUTERMAC_H
