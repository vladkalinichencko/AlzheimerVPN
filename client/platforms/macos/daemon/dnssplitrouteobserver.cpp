#include "dnssplitrouteobserver.h"

#include <QtEndian>

#include "core/utils/dnsMessageParser.h"
#include "logger.h"

using namespace amnezia;

namespace {
    Logger logger("DnsSplitRouteObserver");

    QStringList addressStrings(const QList<QHostAddress> &addresses)
    {
        QStringList result;
        for (const QHostAddress &address : addresses) {
            result.append(address.toString());
        }
        return result;
    }

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
    m_replyTimer.setSingleShot(true);
    m_replyTimer.setInterval(4000);
    connect(&m_replyTimer, &QTimer::timeout, this, &DnsSplitRouteObserver::upstreamUnavailable);
}

void DnsSplitRouteObserver::setRules(const QStringList &rules)
{
    m_rules.clear();
    for (const QString &ruleText : rules) {
        const SplitTunnelRule rule = SplitTunnelRule::fromText(ruleText);
        if (rule.isValid() && rule.type() != SplitTunnelRule::Type::IpSubnet) {
            m_rules.append(rule);
        }
    }
    logger.debug() << "DNS split rules configured input=" << QString::number(rules.size())
                   << "active_host_rules=" << QString::number(m_rules.size());
}

bool DnsSplitRouteObserver::start(const QList<QHostAddress> &upstreamServers)
{
    stop();

    for (const QHostAddress &server : upstreamServers) {
        if (server.protocol() == QAbstractSocket::IPv4Protocol &&
            !server.isLoopback() && !m_upstreamServers.contains(server)) {
            m_upstreamServers.append(server);
        }
    }

    if (m_rules.isEmpty() || m_upstreamServers.isEmpty()) {
        logger.debug() << "DNS split observer not started rules="
                       << QString::number(m_rules.size())
                       << "upstreams=" << addressStrings(m_upstreamServers);
        return false;
    }

    if (!m_upstreamSocket.bind(QHostAddress::AnyIPv4, 0)) {
        logger.warning() << "DNS split observer failed to bind upstream UDP socket:"
                         << m_upstreamSocket.errorString();
        stop();
        return false;
    }

    if (!m_clientSocket.bind(QHostAddress::LocalHost, 53, QUdpSocket::DontShareAddress)) {
        logger.warning() << "DNS split observer failed to bind local UDP 127.0.0.1:53:"
                         << m_clientSocket.errorString();
        stop();
        return false;
    }

    logger.debug() << "DNS split observer started rules=" << QString::number(m_rules.size())
                   << "upstreams=" << addressStrings(m_upstreamServers);
    return true;
}

void DnsSplitRouteObserver::stop()
{
    if (m_traffic.queries || m_traffic.rejectedReplies) {
        logger.info() << "NetTrace DNS proxy queries=" << m_traffic.queries
                      << "forwarded=" << m_traffic.forwarded << "send_errors=" << m_traffic.sendErrors
                      << "replies=" << m_traffic.replies << "rejected_replies=" << m_traffic.rejectedReplies
                      << "client_send_errors=" << m_traffic.clientSendErrors
                      << "pending=" << QString::number(m_pendingQueries.size());
    }
    m_traffic = {};
    m_replyTimer.stop();
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
        ++m_traffic.queries;

        const QString host = DnsMessageParser::queryHost(request);
        if (host.isEmpty() || m_upstreamServers.isEmpty()) {
            logger.debug() << "DNS split UDP query ignored host_empty="
                           << (host.isEmpty() ? "true" : "false")
                           << "upstream_count=" << QString::number(m_upstreamServers.size())
                           << "size=" << QString::number(request.size());
            continue;
        }

        const quint16 originalId = messageId(request);

        PendingQuery pending;
        pending.clientAddress = clientAddress;
        pending.clientPort = clientPort;
        pending.originalId = originalId;
        pending.host = host;
        pending.requestKey = request;
        setMessageId(pending.requestKey, 0);
        pending.matched = matchesRules(host);
        if (!m_replyTimer.isActive()) {
            m_replyTimer.start();
        }
        for (const QHostAddress &upstream : m_upstreamServers) {
            if (m_pendingQueries.size() >= 4096) {
                m_pendingQueries.clear();
            }
            QByteArray forwardedRequest = request;
            const quint16 forwardedId = nextQueryId();
            setMessageId(forwardedRequest, forwardedId);
            if (m_upstreamSocket.writeDatagram(forwardedRequest, upstream, 53) == forwardedRequest.size()) {
                ++m_traffic.forwarded;
                m_pendingQueries.insert(forwardedId, pending);
            } else if (++m_traffic.sendErrors <= 4) {
                logger.warning() << "NetTrace DNS upstream send failed error=" << m_upstreamSocket.error()
                                 << m_upstreamSocket.errorString();
            }
        }
    }
}

void DnsSplitRouteObserver::readUpstreamDatagrams()
{
    while (m_upstreamSocket.hasPendingDatagrams()) {
        QByteArray response;
        response.resize(int(m_upstreamSocket.pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_upstreamSocket.readDatagram(response.data(), response.size(), &sender, &senderPort);

        const quint16 forwardedId = messageId(response);
        if (senderPort != 53 || !m_upstreamServers.contains(sender) ||
            !m_pendingQueries.contains(forwardedId)) {
            ++m_traffic.rejectedReplies;
            continue;
        }

        if (++m_traffic.replies == 1) {
            logger.info() << "NetTrace DNS proxy received first upstream reply after sends=" << m_traffic.forwarded;
        }
        m_replyTimer.stop();
        const PendingQuery pending = m_pendingQueries.take(forwardedId);
        for (auto it = m_pendingQueries.begin(); it != m_pendingQueries.end();) {
            const PendingQuery other = it.value();
            if (other.originalId == pending.originalId &&
                other.clientAddress == pending.clientAddress &&
                other.clientPort == pending.clientPort &&
                other.requestKey == pending.requestKey) {
                it = m_pendingQueries.erase(it);
            } else {
                ++it;
            }
        }
        const QList<DnsIpv4Answer> answers = DnsMessageParser::responseIpv4Answers(response);
        if (pending.matched && !answers.isEmpty()) {
            emit hostResolved(pending.host, answers);
        }

        setMessageId(response, pending.originalId);
        if (m_clientSocket.writeDatagram(response, pending.clientAddress, pending.clientPort) != response.size()) {
            ++m_traffic.clientSendErrors;
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
