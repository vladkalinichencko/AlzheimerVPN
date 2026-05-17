#include "splitTunnelRule.h"

#include "core/utils/networkUtilities.h"

using namespace amnezia;

namespace {
    QString normalizeHostText(QString text)
    {
        text = text.trimmed().toLower();
        text.replace("https://", "");
        text.replace("http://", "");
        text.replace("ftp://", "");
        text = text.split("/", Qt::SkipEmptyParts).first();
        return text;
    }

    QRegularExpression wildcardToRegularExpression(const QString &pattern)
    {
        QString regex = QRegularExpression::escape(pattern);
        regex.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
        return QRegularExpression(QStringLiteral("^") + regex + QStringLiteral("$"));
    }
}

SplitTunnelRule::SplitTunnelRule(Type type, const QString &value, const QString &normalizedText)
    : m_type(type),
      m_value(value),
      m_normalizedText(normalizedText)
{
    if (m_type == Type::WildcardHost) {
        m_regex = wildcardToRegularExpression(m_value);
    }
}

SplitTunnelRule SplitTunnelRule::fromText(const QString &text)
{
    const QString normalized = normalizeHostText(text);

    if (NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(normalized)) {
        return SplitTunnelRule(Type::IpSubnet, normalized, normalized);
    }

    if (normalized.contains("*")) {
        return SplitTunnelRule(Type::WildcardHost, normalized, normalized);
    }

    return SplitTunnelRule(Type::ExactHost, normalized, normalized);
}

bool SplitTunnelRule::isValid() const
{
    if (m_value.isEmpty()) {
        return false;
    }

    if (m_type == Type::IpSubnet) {
        return NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(m_value);
    }

    if (m_type == Type::WildcardHost) {
        const QString domainProbe = m_value;
        return m_regex.isValid() &&
               m_value.contains(".") &&
               NetworkUtilities::domainRegExp().exactMatch(QString(domainProbe).replace("*", "a"));
    }

    return NetworkUtilities::domainRegExp().exactMatch(m_value);
}

bool SplitTunnelRule::isDynamicHostRule() const
{
    return m_type == Type::WildcardHost;
}

bool SplitTunnelRule::matchesHost(const QString &host) const
{
    const QString normalizedHost = normalizeHostText(host);

    switch (m_type) {
    case Type::ExactHost:
        return normalizedHost == m_value;
    case Type::WildcardHost:
        return m_regex.match(normalizedHost).hasMatch();
    case Type::IpSubnet:
        return false;
    }

    return false;
}

SplitTunnelRule::Type SplitTunnelRule::type() const
{
    return m_type;
}

QString SplitTunnelRule::value() const
{
    return m_value;
}

QString SplitTunnelRule::normalizedText() const
{
    return m_normalizedText;
}
