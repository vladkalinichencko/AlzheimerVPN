#ifndef DNSMESSAGEPARSER_H
#define DNSMESSAGEPARSER_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace amnezia
{
    class DnsMessageParser
    {
    public:
        static QString queryHost(const QByteArray &message);
        static QStringList responseIpv4Addresses(const QByteArray &message);

    private:
        static bool readName(const QByteArray &message, int &offset, QString &name, int depth = 0);
    };
}

#endif // DNSMESSAGEPARSER_H
