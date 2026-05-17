#include "dnsMessageParser.h"

#include <QtEndian>

using namespace amnezia;

namespace {
    quint16 readUint16(const QByteArray &message, int offset)
    {
        return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(message.constData() + offset));
    }
}

QString DnsMessageParser::queryHost(const QByteArray &message)
{
    if (message.size() < 12 || readUint16(message, 4) == 0) {
        return {};
    }

    int offset = 12;
    QString host;
    if (!readName(message, offset, host)) {
        return {};
    }
    return host.toLower();
}

QStringList DnsMessageParser::responseIpv4Addresses(const QByteArray &message)
{
    QStringList result;
    if (message.size() < 12) {
        return result;
    }

    int offset = 12;
    const quint16 questionCount = readUint16(message, 4);
    const quint16 answerCount = readUint16(message, 6);
    const quint16 authorityCount = readUint16(message, 8);
    const quint16 additionalCount = readUint16(message, 10);

    for (quint16 i = 0; i < questionCount; ++i) {
        QString ignoredName;
        if (!readName(message, offset, ignoredName) || offset + 4 > message.size()) {
            return {};
        }
        offset += 4;
    }

    const quint16 recordCount = answerCount + authorityCount + additionalCount;
    for (quint16 i = 0; i < recordCount; ++i) {
        QString ignoredName;
        if (!readName(message, offset, ignoredName) || offset + 10 > message.size()) {
            return result;
        }

        const quint16 type = readUint16(message, offset);
        const quint16 klass = readUint16(message, offset + 2);
        const quint16 dataLength = readUint16(message, offset + 8);
        offset += 10;

        if (offset + dataLength > message.size()) {
            return result;
        }

        if (type == 1 && klass == 1 && dataLength == 4) {
            const uchar *p = reinterpret_cast<const uchar *>(message.constData() + offset);
            result.append(QStringLiteral("%1.%2.%3.%4").arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]));
        }
        offset += dataLength;
    }

    result.removeDuplicates();
    return result;
}

bool DnsMessageParser::readName(const QByteArray &message, int &offset, QString &name, int depth)
{
    if (depth > 8 || offset < 0 || offset >= message.size()) {
        return false;
    }

    QStringList labels;
    int currentOffset = offset;
    bool jumped = false;

    while (currentOffset < message.size()) {
        const uchar length = static_cast<uchar>(message.at(currentOffset));
        if (length == 0) {
            currentOffset++;
            if (!jumped) {
                offset = currentOffset;
            }
            name = labels.join(".");
            return !name.isEmpty();
        }

        if ((length & 0xC0) == 0xC0) {
            if (currentOffset + 1 >= message.size()) {
                return false;
            }
            const int pointer = ((length & 0x3F) << 8) | static_cast<uchar>(message.at(currentOffset + 1));
            if (!jumped) {
                offset = currentOffset + 2;
            }
            currentOffset = pointer;
            jumped = true;
            depth++;
            if (depth > 8) {
                return false;
            }
            continue;
        }

        if ((length & 0xC0) != 0 || currentOffset + 1 + length > message.size()) {
            return false;
        }

        labels.append(QString::fromLatin1(message.mid(currentOffset + 1, length)));
        currentOffset += 1 + length;
    }

    return false;
}
