#ifndef DNSSPLITROUTEOBSERVER_H
#define DNSSPLITROUTEOBSERVER_H

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QStringList>
#include <QUdpSocket>

#include "core/utils/splitTunnelRule.h"

class DnsSplitRouteObserver final : public QObject
{
    Q_OBJECT

public:
    explicit DnsSplitRouteObserver(QObject *parent = nullptr);

    void setRules(const QStringList &rules);
    bool start(const QList<QHostAddress> &upstreamServers);
    void stop();
    bool isActive() const;

signals:
    void hostResolved(const QString &host, const QStringList &ips);

private slots:
    void readClientDatagrams();
    void readUpstreamDatagrams();

private:
    struct PendingQuery {
        QHostAddress clientAddress;
        quint16 clientPort = 0;
        quint16 originalId = 0;
        QString host;
        bool matched = false;
    };

    bool matchesRules(const QString &host) const;
    quint16 nextQueryId();

    QUdpSocket m_clientSocket;
    QUdpSocket m_upstreamSocket;
    QList<QHostAddress> m_upstreamServers;
    QList<amnezia::SplitTunnelRule> m_rules;
    QHash<quint16, PendingQuery> m_pendingQueries;
    quint16 m_nextQueryId = 1;
};

#endif // DNSSPLITROUTEOBSERVER_H
