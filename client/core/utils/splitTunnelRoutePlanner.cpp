#include "splitTunnelRoutePlanner.h"

#include <QHostAddress>

#include "core/utils/networkUtilities.h"
#include "core/utils/splitTunnelRule.h"

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

QStringList SplitTunnelRoutePlanner::cachedSiteIps(const QVariantMap &sites)
{
    QStringList ips;
    for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
        const SplitTunnelRule rule = SplitTunnelRule::fromText(it.key());
        if (!rule.isValid()) {
            continue;
        }
        if (rule.type() == SplitTunnelRule::Type::IpSubnet) {
            ips.append(rule.normalizedText());
        } else {
            ips.append(it.value().toStringList());
        }
    }

    QStringList result;
    for (const QString &ip : std::as_const(ips)) {
        if (NetworkUtilities::checkIpSubnetFormat(ip) && !result.contains(ip)) {
            result.append(ip);
        }
    }
    return result;
}
