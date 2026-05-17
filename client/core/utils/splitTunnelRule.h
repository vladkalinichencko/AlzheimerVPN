#ifndef SPLITTUNNELRULE_H
#define SPLITTUNNELRULE_H

#include <QRegularExpression>
#include <QString>

namespace amnezia
{
    class SplitTunnelRule
    {
    public:
        enum class Type {
            ExactHost,
            WildcardHost,
            IpSubnet
        };

        static SplitTunnelRule fromText(const QString &text);

        bool isValid() const;
        bool isDynamicHostRule() const;
        bool matchesHost(const QString &host) const;

        Type type() const;
        QString value() const;
        QString normalizedText() const;

    private:
        SplitTunnelRule(Type type, const QString &value, const QString &normalizedText);

        Type m_type;
        QString m_value;
        QString m_normalizedText;
        QRegularExpression m_regex;
    };
}

#endif // SPLITTUNNELRULE_H
