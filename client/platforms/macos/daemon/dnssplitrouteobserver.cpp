#include "dnssplitrouteobserver.h"

#include <QtEndian>

#include "core/utils/dnsMessageParser.h"

using namespace amnezia;

namespace {
    quint16 messageId(const QByteArray &message)
    {
        if (message.size() < 2) {
            return 0;
        }
        return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(message.constData()));
    }

    void setMessageId(QByteArray &message, quint16 id)
    {
        if (message.size() < 2) {
            return;
        }
        qToBigEndian<quint16>(id, reinterpret_cast<uchar *>(message.data()));
    }
}

DnsSplitRouteObserver::DnsSplitRouteObserver(QObject *parent)
    : QObject(parent)
{
    connect(&m_clientSocket, &QUdpSocket::readyRead, this, &DnsSplitRouteObserver::readClientDatagrams);
    connect(&m_upstreamSocket, &QUdpSocket::readyRead, this, &DnsSplitRouteObserver::readUpstreamDatagrams);
}

void DnsSplitRouteObserver::setRules(const QStringList &rules)
{
    m_rules.clear();
    for (const QString &ruleText : rules) {
        const SplitTunnelRule rule = SplitTunnelRule::fromText(ruleText);
        if (rule.isValid() && rule.type() == SplitTunnelRule::Type::WildcardHost) {
            m_rules.append(rule);
        }
    }
}

bool DnsSplitRouteObserver::start(const QList<QHostAddress> &upstreamServers)
{
    stop();

    for (const QHostAddress &server : upstreamServers) {
        if (server.protocol() == QAbstractSocket::IPv4Protocol && server != QHostAddress::LocalHost) {
            m_upstreamServers.append(server);
        }
    }

    if (m_rules.isEmpty() || m_upstreamServers.isEmpty()) {
        return false;
    }

    if (!m_upstreamSocket.bind(QHostAddress::AnyIPv4, 0)) {
        stop();
        return false;
    }

    const auto bindMode = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    if (!m_clientSocket.bind(QHostAddress::LocalHost, 53, bindMode)) {
        stop();
        return false;
    }

    return true;
}

void DnsSplitRouteObserver::stop()
{
    m_clientSocket.close();
    m_upstreamSocket.close();
    m_upstreamServers.clear();
    m_pendingQueries.clear();
}

bool DnsSplitRouteObserver::isActive() const
{
    return m_clientSocket.state() == QAbstractSocket::BoundState &&
           m_upstreamSocket.state() == QAbstractSocket::BoundState;
}

void DnsSplitRouteObserver::readClientDatagrams()
{
    while (m_clientSocket.hasPendingDatagrams()) {
        QHostAddress clientAddress;
        quint16 clientPort = 0;
        QByteArray request;
        request.resize(int(m_clientSocket.pendingDatagramSize()));
        m_clientSocket.readDatagram(request.data(), request.size(), &clientAddress, &clientPort);

        const QString host = DnsMessageParser::queryHost(request);
        if (host.isEmpty() || m_upstreamServers.isEmpty()) {
            continue;
        }

        const quint16 originalId = messageId(request);
        const quint16 forwardedId = nextQueryId();
        setMessageId(request, forwardedId);

        PendingQuery pending;
        pending.clientAddress = clientAddress;
        pending.clientPort = clientPort;
        pending.originalId = originalId;
        pending.host = host;
        pending.matched = matchesRules(host);
        m_pendingQueries.insert(forwardedId, pending);

        m_upstreamSocket.writeDatagram(request, m_upstreamServers.first(), 53);
    }
}

void DnsSplitRouteObserver::readUpstreamDatagrams()
{
    while (m_upstreamSocket.hasPendingDatagrams()) {
        QByteArray response;
        response.resize(int(m_upstreamSocket.pendingDatagramSize()));
        m_upstreamSocket.readDatagram(response.data(), response.size());

        const quint16 forwardedId = messageId(response);
        if (!m_pendingQueries.contains(forwardedId)) {
            continue;
        }

        const PendingQuery pending = m_pendingQueries.take(forwardedId);
        const QStringList ips = DnsMessageParser::responseIpv4Addresses(response);
        setMessageId(response, pending.originalId);
        m_clientSocket.writeDatagram(response, pending.clientAddress, pending.clientPort);

        if (pending.matched && !ips.isEmpty()) {
            emit hostResolved(pending.host, ips);
        }
    }
}

bool DnsSplitRouteObserver::matchesRules(const QString &host) const
{
    for (const SplitTunnelRule &rule : m_rules) {
        if (rule.matchesHost(host)) {
            return true;
        }
    }
    return false;
}

quint16 DnsSplitRouteObserver::nextQueryId()
{
    do {
        ++m_nextQueryId;
        if (m_nextQueryId == 0) {
            ++m_nextQueryId;
        }
    } while (m_pendingQueries.contains(m_nextQueryId));

    return m_nextQueryId;
}
