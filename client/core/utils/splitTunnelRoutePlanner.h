#ifndef SPLITTUNNELROUTEPLANNER_H
#define SPLITTUNNELROUTEPLANNER_H

#include <QHostInfo>
#include <QStringList>
#include <QVariantMap>

#include "core/utils/routeModes.h"

namespace amnezia
{
    class SplitTunnelRoutePlanner
    {
    public:
        static QStringList resolvedIpv4Routes(const QHostInfo &hostInfo, const QStringList &alreadyRoutedIps = {});
        static bool shouldRetryRouteAdd(const QStringList &routeIps, bool routesAdded, bool canRetry);
        static QStringList killSwitchAllowedIps(RouteMode mode, bool killSwitchEnabled, const QStringList &routeIps);
        static QStringList cachedSiteIps(const QVariantMap &sites);
    };
}

#endif // SPLITTUNNELROUTEPLANNER_H
