#include "splitTunnelRoutePlanner.h"

#include <QHostAddress>

using namespace amnezia;

QStringList SplitTunnelRoutePlanner::resolvedIpv4Routes(const QHostInfo &hostInfo, const QStringList &alreadyRoutedIps)
{
    QStringList result;
    for (const QHostAddress &addr : hostInfo.addresses()) {
        if (addr.protocol() == QAbstractSocket::NetworkLayerProtocol::IPv4Protocol) {
            const QString ip = addr.toString();
            if (!alreadyRoutedIps.contains(ip)) {
                result.append(ip);
            }
        }
    }
    result.removeDuplicates();
    return result;
}

bool SplitTunnelRoutePlanner::shouldRetryRouteAdd(const QStringList &routeIps, bool routesAdded, bool canRetry)
{
    return !routeIps.isEmpty() && !routesAdded && canRetry;
}

QStringList SplitTunnelRoutePlanner::killSwitchAllowedIps(RouteMode mode, bool killSwitchEnabled, const QStringList &routeIps)
{
    if (mode == RouteMode::VpnAllExceptSites && killSwitchEnabled) {
        return routeIps;
    }
    return {};
}
