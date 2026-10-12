#include "ipSplitTunnelingController.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/splitTunnelRule.h"
#include <QJsonObject>
#include <QDebug>

IpSplitTunnelingController::IpSplitTunnelingController(SecureAppSettingsRepository* appSettingsRepository, QObject* parent)
    : QObject(parent),
      m_appSettingsRepository(appSettingsRepository)
{
    m_currentRouteMode = m_appSettingsRepository->routeMode();
    if (m_currentRouteMode == RouteMode::VpnAllSites) { // for old split tunneling configs
        m_appSettingsRepository->setRouteMode(RouteMode::VpnOnlyForwardSites);
        m_currentRouteMode = RouteMode::VpnOnlyForwardSites;
    }
    fillSites();
}

IpSplitTunnelingController::~IpSplitTunnelingController()
{
    clearChecks();
}

void IpSplitTunnelingController::clearChecks()
{
    for (const SiteCheck &check : m_siteChecks) {
        if (check.lookupId >= 0) {
            QHostInfo::abortHostLookup(check.lookupId);
        }
    }
    m_siteChecks.clear();
}

IpSplitTunnelingController::SiteCheck IpSplitTunnelingController::siteCheck(const QString &hostname) const
{
    const SplitTunnelRule rule = SplitTunnelRule::fromText(hostname);
    if (rule.type() == SplitTunnelRule::Type::IpSubnet) {
        return {CheckState::Address, {}, -1};
    }
    if (rule.isDynamicHostRule()) {
        return {CheckState::Dynamic, {}, -1};
    }
    return m_siteChecks.value(rule.normalizedText());
}

void IpSplitTunnelingController::checkSite(const QString &hostname)
{
    const SplitTunnelRule rule = SplitTunnelRule::fromText(hostname);
    const QString host = rule.normalizedText();
    if (rule.type() != SplitTunnelRule::Type::ExactHost ||
        !m_appSettingsRepository->vpnSites(m_currentRouteMode).contains(host)) {
        return;
    }
    SiteCheck &check = m_siteChecks[host];
    if (check.state == CheckState::Resolving) {
        return;
    }
    check = {CheckState::Resolving, {}, -1};
    check.lookupId = QHostInfo::lookupHost(host, this, [this, host](const QHostInfo &info) {
        onHostResolved(host, info);
    });
    emit siteCheckChanged(host);
}

bool IpSplitTunnelingController::addSiteInternal(const QString &hostname, const QStringList &ips)
{
    QVariantMap existing = m_appSettingsRepository->vpnSites(m_currentRouteMode);
    if (existing.contains(hostname) && ips.isEmpty()) {
        return false;
    }

    for (int i = 0; i < m_sites.size(); i++) {
        if (m_sites[i].first == hostname) {
            bool changed = false;
            for (const QString &ip : ips) {
                if (!ip.isEmpty() && !m_sites[i].second.contains(ip)) {
                    m_sites[i].second.append(ip);
                    changed = true;
                }
            }
            if (!changed) {
                return false;
            }
            m_appSettingsRepository->addVpnSite(m_currentRouteMode, hostname, ips);
            return true;
        }
    }
    m_sites.append(qMakePair(hostname, ips));
    m_appSettingsRepository->addVpnSite(m_currentRouteMode, hostname, ips);
    return true;
}

void IpSplitTunnelingController::addSites(const QMap<QString, QStringList> &sites, bool replaceExisting)
{
    QMap<QString, QStringList> normalizedSites;
    for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
        const SplitTunnelRule rule = SplitTunnelRule::fromText(it.key());
        const QString hostname = rule.normalizedText();

        if (!validateHostname(hostname)) {
            qDebug() << hostname << " not look like ip adress or domain name";
            continue;
        }

        QStringList &normalizedIps = normalizedSites[hostname];
        for (const QString &ip : it.value()) {
            const QString normalizedIp = ip.trimmed();
            if (!normalizedIp.isEmpty() && !normalizedIps.contains(normalizedIp)) {
                normalizedIps.append(normalizedIp);
            }
        }
    }

    if (replaceExisting) {
        clearChecks();
        m_sites.clear();
    }
    for (auto it = normalizedSites.constBegin(); it != normalizedSites.constEnd(); ++it) {
        const QString &hostname = it.key();
        const QStringList &ips = it.value();
        bool found = false;
        for (int i = 0; i < m_sites.size(); i++) {
            if (m_sites[i].first == hostname) {
                for (const QString &ip : ips) {
                    if (!ip.isEmpty() && !m_sites[i].second.contains(ip)) {
                        m_sites[i].second.append(ip);
                    }
                }
                found = true;
                break;
            }
        }
        if (!found) {
            m_sites.append(qMakePair(hostname, ips));
        }
    }
    if (replaceExisting) {
        m_appSettingsRepository->removeAllVpnSites(m_currentRouteMode);
    }
    m_appSettingsRepository->addVpnSites(m_currentRouteMode, normalizedSites);
    for (auto it = normalizedSites.constBegin(); it != normalizedSites.constEnd(); ++it) {
        if (it.value().isEmpty()) {
            checkSite(it.key());
        }
    }
}

bool IpSplitTunnelingController::addSite(const QString &hostname)
{
    const SplitTunnelRule rule = SplitTunnelRule::fromText(hostname);
    QString normalizedHostname = rule.normalizedText();
    
    if (!validateHostname(normalizedHostname)) {
        return false;
    }
    
    if (rule.type() == SplitTunnelRule::Type::IpSubnet || rule.isDynamicHostRule()) {
        processSite(normalizedHostname, {});
        return true;
    }
    
    if (addSiteInternal(normalizedHostname, {})) {
        checkSite(normalizedHostname);
        return true;
    }
    
    return false;
}

bool IpSplitTunnelingController::removeSite(const QString &hostname)
{
    for (int i = 0; i < m_sites.size(); i++) {
        if (m_sites[i].first == hostname) {
            const SiteCheck check = m_siteChecks.take(hostname);
            if (check.lookupId >= 0) {
                QHostInfo::abortHostLookup(check.lookupId);
            }
            m_sites.removeAt(i);
            m_appSettingsRepository->removeVpnSite(m_currentRouteMode, hostname);
            return true;
        }
    }
    return false;
}

void IpSplitTunnelingController::removeSites()
{
    clearChecks();
    m_sites.clear();
    m_appSettingsRepository->removeAllVpnSites(m_currentRouteMode);
}

void IpSplitTunnelingController::setRouteMode(RouteMode routeMode)
{
    clearChecks();
    m_currentRouteMode = routeMode;
    fillSites();
    m_appSettingsRepository->setRouteMode(routeMode);
}

void IpSplitTunnelingController::toggleSplitTunneling(bool enabled)
{
    m_appSettingsRepository->setSitesSplitTunnelingEnabled(enabled);
}

RouteMode IpSplitTunnelingController::getRouteMode() const
{
    return m_currentRouteMode;
}

bool IpSplitTunnelingController::isSplitTunnelingEnabled() const
{
    return m_appSettingsRepository->isSitesSplitTunnelingEnabled();
}

QVector<QPair<QString, QStringList>> IpSplitTunnelingController::getCurrentSites() const
{
    return m_sites;
}

void IpSplitTunnelingController::fillSites()
{
    QVariantMap sitesMap = m_appSettingsRepository->vpnSites(m_currentRouteMode);
    QVariantMap normalizedSitesMap;
    m_sites.clear();
    for (auto it = sitesMap.begin(); it != sitesMap.end(); ++it) {
        const SplitTunnelRule rule = SplitTunnelRule::fromText(it.key());
        const QString hostname = rule.normalizedText();

        if (!validateHostname(hostname)) {
            qDebug() << hostname << " not look like ip adress or domain name";
            continue;
        }

        QStringList ips = SecureAppSettingsRepository::siteIpList(normalizedSitesMap.value(hostname));
        for (const QString &ip : SecureAppSettingsRepository::siteIpList(it.value())) {
            const QString normalizedIp = ip.trimmed();
            if (!normalizedIp.isEmpty() && !ips.contains(normalizedIp)) {
                ips.append(normalizedIp);
            }
        }
        normalizedSitesMap.insert(hostname, ips);
    }
    for (auto it = normalizedSitesMap.begin(); it != normalizedSitesMap.end(); ++it) {
        m_sites.append(qMakePair(it.key(), SecureAppSettingsRepository::siteIpList(it.value())));
    }
    if (normalizedSitesMap != sitesMap) {
        QMap<QString, QStringList> normalizedSites;
        for (auto it = normalizedSitesMap.begin(); it != normalizedSitesMap.end(); ++it) {
            normalizedSites.insert(it.key(), SecureAppSettingsRepository::siteIpList(it.value()));
        }
        m_appSettingsRepository->removeAllVpnSites(m_currentRouteMode);
        m_appSettingsRepository->addVpnSites(m_currentRouteMode, normalizedSites);
    }
}

QString IpSplitTunnelingController::normalizeHostname(const QString &hostname) const
{
    QString normalized = hostname;
    normalized.replace("https://", "");
    normalized.replace("http://", "");
    normalized.replace("ftp://", "");

    if (NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(normalized)) {
        return normalized;
    }

    const QStringList parts = normalized.split("/", Qt::SkipEmptyParts);
    return parts.isEmpty() ? QString() : parts.first();
}

bool IpSplitTunnelingController::validateHostname(const QString &hostname) const
{
    return SplitTunnelRule::fromText(hostname).isValid();
}


void IpSplitTunnelingController::onHostResolved(const QString &hostname, const QHostInfo &hostInfo)
{
    auto it = m_siteChecks.find(hostname);
    if (it == m_siteChecks.end() || it->state != CheckState::Resolving ||
        it->lookupId != hostInfo.lookupId()) {
        return;
    }
    const QList<QHostAddress> &addresses = hostInfo.addresses();

    QStringList allIpv4;
    for (const QHostAddress &addr : addresses) {
        if (addr.protocol() == QAbstractSocket::NetworkLayerProtocol::IPv4Protocol) {
            allIpv4.append(addr.toString());
        }
    }
    allIpv4.removeDuplicates();
    qDebug() << "[SplitTunneling] Host resolved:" << hostname
             << "-> adding all IPv4 addresses to list:" << allIpv4;

    if (hostInfo.error() == QHostInfo::NoError && !allIpv4.isEmpty()) {
        *it = {CheckState::Resolved, {}, -1};
        processSiteAfterResolve(hostname, allIpv4);
    } else {
        const QString error = hostInfo.error() == QHostInfo::NoError
            ? tr("No IPv4 addresses returned") : hostInfo.errorString();
        *it = {CheckState::Failed, error, -1};
    }
    emit siteCheckChanged(hostname);
}

void IpSplitTunnelingController::processSiteAfterResolve(const QString &hostname, const QStringList &ips)
{
    for (int i = 0; i < m_sites.size(); i++) {
        if (m_sites[i].first == hostname) {
            for (const QString &ip : ips) {
                if (!ip.isEmpty() && !m_sites[i].second.contains(ip)) {
                    m_sites[i].second.append(ip);
                }
            }
            break;
        }
    }
    m_appSettingsRepository->addVpnSite(m_currentRouteMode, hostname, ips);
}

void IpSplitTunnelingController::processSite(const QString &hostname, const QStringList &ips)
{
    addSiteInternal(hostname, ips);
}

bool IpSplitTunnelingController::importSitesFromJson(const QByteArray& jsonData, bool replaceExisting, QString &errorMessage)
{
    QJsonParseError parseError;
    QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonData, &parseError);
    
    if (parseError.error != QJsonParseError::NoError) {
        errorMessage = tr("Failed to parse JSON data: %1").arg(parseError.errorString());
        return false;
    }
    
    if (!jsonDocument.isArray()) {
        errorMessage = tr("The JSON data is not an array");
        return false;
    }
    
    QJsonArray jsonArray = jsonDocument.array();
    QMap<QString, QStringList> sites;
    
    for (auto jsonValue : jsonArray) {
        QJsonObject jsonObject = jsonValue.toObject();
        QString hostname = jsonObject.value("hostname").toString("").trimmed();

        QStringList ips;
        if (jsonObject.value("ips").isArray()) {
            const QJsonArray ipsArray = jsonObject.value("ips").toArray();
            for (const auto &ipValue : ipsArray) {
                ips.append(ipValue.toString());
            }
        }
        const QString singleIp = jsonObject.value("ip").toString("");
        if (!singleIp.isEmpty()) {
            ips.append(singleIp);
        }
        ips.removeAll(QString());
        ips.removeDuplicates();
        
        const SplitTunnelRule rule = SplitTunnelRule::fromText(hostname);
        const QString normalizedHostname = rule.normalizedText();
        
        if (!validateHostname(normalizedHostname)) {
            qDebug() << normalizedHostname << " not look like ip adress or domain name";
            continue;
        }
        
        sites.insert(normalizedHostname, ips);
    }
    
    addSites(sites, replaceExisting);
    
    return true;
}

QByteArray IpSplitTunnelingController::exportSitesToJson() const
{
    QVector<QPair<QString, QStringList>> sites = getCurrentSites();
    QJsonArray jsonArray;
    
    for (const auto &site : sites) {
        QJsonObject jsonObject;
        jsonObject["hostname"] = site.first;

        QJsonArray ipsArray;
        for (const QString &ip : site.second) {
            ipsArray.append(ip);
        }
        jsonObject["ips"] = ipsArray;
        // Keep the legacy "ip" field (first address) for backward compatibility.
        jsonObject["ip"] = site.second.isEmpty() ? QString() : site.second.first();

        jsonArray.append(jsonObject);
    }
    
    QJsonDocument jsonDocument(jsonArray);
    return jsonDocument.toJson();
}
