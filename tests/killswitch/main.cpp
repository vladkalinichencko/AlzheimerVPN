#include <QCoreApplication>
#include <QJsonArray>
#include <QSet>
#include <cstdlib>

#include "../../service/server/killswitch.h"
#include "platforms/macos/daemon/macosfirewall.h"
#include "core/utils/networkUtilities.h"

static QSet<QString> anchors;
static QStringList allowed;
static QStringList blocked;
static bool installed = false;
static bool installFails = false;
static bool strict = false;

void MacOSFirewall::install() { installed = !installFails; }
void MacOSFirewall::uninstall() { installed = false; anchors.clear(); }
bool MacOSFirewall::isInstalled() { return installed; }
void MacOSFirewall::ensureRootAnchorPriority() {}
bool MacOSFirewall::isAnchorEnabled(const QString &anchor) { return anchors.contains(anchor); }
void MacOSFirewall::setAnchorEnabled(const QString &anchor, bool enabled)
{
    if (enabled) anchors.insert(anchor);
    else anchors.remove(anchor);
}
void MacOSFirewall::setAnchorTable(const QString &, bool enabled, const QString &table, const QStringList &items)
{
    if (table == "allownets") allowed = items;
    if (table == "blocknets") blocked = enabled ? items : QStringList();
}

SecureQSettings::SecureQSettings(const QString &, const QString &, QObject *parent, bool)
    : QObject(parent), m_settings(QSettings::IniFormat, QSettings::UserScope, "KillSwitchTest", "KillSwitchTest") {}
QVariant SecureQSettings::value(const QString &, const QVariant &) const { return strict; }
void SecureQSettings::setValue(const QString &, const QVariant &value) { strict = value.toBool(); }
bool NetworkUtilities::checkIpSubnetFormat(const QString &ip)
{
    return QHostAddress::parseSubnet(ip).second >= 0;
}

static void check(bool condition, const char *message)
{
    if (!condition) { qFatal("%s", message); }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto *killSwitch = KillSwitch::instance();
    check(killSwitch->init(), "initialization failed");
    QJsonObject config {{"vpnServer", "203.0.113.10"}, {"dns1", "1.1.1.1"}, {"splitTunnelType", 0}};

    check(killSwitch->setConnectionProtection(true, config), "connection protection did not arm");
    check(installed && anchors.contains("100.blockAll"), "startup traffic is not blocked");
    for (int attempt = 0; attempt < 3; ++attempt) {
        check(killSwitch->disableKillSwitch(), "protocol cleanup failed");
        check(installed && anchors.contains("100.blockAll"), "recovery removed protection");
        check(killSwitch->enableKillSwitch(config, 0), "reconnect rules failed");
    }
    check(killSwitch->setConnectionProtection(false, {}), "manual disconnect failed");
    check(!installed, "manual disconnect kept ordinary protection");

    config.insert("splitTunnelType", 2);
    config.insert("splitTunnelSites", QJsonArray {"198.51.100.2"});
    check(killSwitch->setConnectionProtection(true, config), "split protection did not arm");
    check(allowed.contains("203.0.113.10") && allowed.contains("198.51.100.2"), "split exceptions were lost");
    killSwitch->addAllowedRange({"198.51.100.3"});
    check(allowed.contains("203.0.113.10") && allowed.contains("198.51.100.2")
          && allowed.contains("198.51.100.3"), "DNS update discarded the VPN endpoint or static exceptions");
    killSwitch->disableKillSwitch();
    check(installed && anchors.contains("100.blockAll"), "split recovery removed protection");
    killSwitch->setConnectionProtection(false, {});

    config.insert("splitTunnelType", 1);
    check(killSwitch->setConnectionProtection(true, config), "include-only protection did not arm");
    check(anchors.contains("120.blockNets") && !anchors.contains("100.blockAll"), "include-only policy changed");
    killSwitch->disableKillSwitch();
    check(anchors.contains("120.blockNets"), "include-only recovery removed protection");
    killSwitch->setConnectionProtection(false, {});

    config.insert("splitTunnelType", 0);
    check(!killSwitch->setAlwaysBlockedSites({"not-an-ip"}), "invalid always-blocked site was accepted");
    check(!installed, "invalid always-blocked site touched the firewall");
    check(killSwitch->setAlwaysBlockedSites({"203.0.113.77", "198.51.100.0/24"}), "always-blocked sites did not arm");
    check(installed && anchors.contains("120.blockNets") && anchors.contains("200.allowVPN")
          && !anchors.contains("100.blockAll"), "idle block changed ordinary traffic");
    check(blocked == QStringList({"203.0.113.77", "198.51.100.0/24"}), "idle block table is wrong");
    check(killSwitch->setConnectionProtection(true, config), "protection failed with always-blocked sites");
    check(anchors.contains("100.blockAll") && !anchors.contains("120.blockNets"),
          "protection did not replace the idle block");
    check(killSwitch->setAlwaysBlockedSites({"203.0.113.88"}), "update during protection failed");
    check(anchors.contains("100.blockAll") && !anchors.contains("120.blockNets"),
          "update during protection touched the connection rules");
    killSwitch->disableKillSwitch();
    check(anchors.contains("100.blockAll"), "protocol cleanup released the connection protection");
    check(killSwitch->setConnectionProtection(false, {}), "disconnect failed with always-blocked sites");
    check(installed && anchors.contains("120.blockNets") && !anchors.contains("100.blockAll")
          && blocked == QStringList({"203.0.113.88"}), "disconnect did not return to the always-blocked sites");
    check(killSwitch->setAlwaysBlockedSites({}), "release failed");
    check(!installed, "release kept the firewall installed");

    strict = true;
    killSwitch->setConnectionProtection(true, config);
    killSwitch->setConnectionProtection(false, {});
    check(installed && anchors.contains("100.blockAll"), "manual disconnect bypassed strict protection");
    strict = false;
    killSwitch->disableKillSwitch();

    installFails = true;
    check(!killSwitch->setConnectionProtection(true, config), "firewall failure was reported as success");
    killSwitch->disableKillSwitch();
    check(anchors.contains("100.blockAll"), "failed startup cleanup discarded partial protection");
    killSwitch->setConnectionProtection(false, {});
    installFails = false;
    config.insert("dns1", "invalid");
    check(!killSwitch->setConnectionProtection(true, config), "invalid config was accepted");
    qInfo("Connection kill switch regression checks passed");
}
