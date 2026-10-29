#ifndef KILLSWITCH_H
#define KILLSWITCH_H

#include <QJsonObject>
#include <QSharedPointer>

#include "secureQSettings.h"

class KillSwitch : public QObject
{
    Q_OBJECT
public:
    static KillSwitch *instance();
    bool init();
    bool refresh(bool enabled);
    bool disableKillSwitch();
    bool setConnectionProtection(bool enabled, const QJsonObject &config);
    bool disableAllTraffic();
    bool enablePeerTraffic(const QJsonObject &configStr);
    bool enableKillSwitch(const QJsonObject &configStr, int vpnAdapterIndex);
    bool resetAllowedRange(const QStringList &ranges);
    bool addAllowedRange(const QStringList &ranges);
    bool removeAllowedRange(const QStringList &ranges);
    bool isStrictKillSwitchEnabled();
    bool setAlwaysBlockedSites(const QStringList &ranges);

private:
    KillSwitch(QObject* parent) {};
    void applyAlwaysBlockedSites();
    QStringList m_allowedRanges;
    QStringList m_alwaysBlockedSites;
    bool m_connectionProtection = false;
    QSharedPointer<SecureQSettings> m_appSettigns;

};

#endif // KILLSWITCH_H
