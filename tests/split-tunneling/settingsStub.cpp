#include "secureQSettings.h"

SecureQSettings::SecureQSettings(const QString &, const QString &, QObject *parent, bool)
    : QObject(parent), m_encryptionEnabled(false) {}
QVariant SecureQSettings::value(const QString &key, const QVariant &fallback) const { return m_cache.value(key, fallback); }
void SecureQSettings::setValue(const QString &key, const QVariant &value) { m_cache.insert(key, value); }
void SecureQSettings::remove(const QString &key) { m_cache.remove(key); }
void SecureQSettings::clearSettings() { m_cache.clear(); }
QByteArray SecureQSettings::backupAppConfig() const { return {}; }
bool SecureQSettings::restoreAppConfig(const QByteArray &) { return false; }
