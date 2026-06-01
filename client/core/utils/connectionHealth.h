#ifndef CONNECTIONHEALTH_H
#define CONNECTIONHEALTH_H

#include <QObject>
#include <QString>

#include "core/protocols/vpnProtocol.h"

namespace amnezia
{
namespace connection_health_ns
{
Q_NAMESPACE

enum ConnectionHealth {
    Unknown,
    Idle,
    PreparingConfig,
    CheckingLocalNetwork,
    CheckingApi,
    CheckingLocalService,
    StartingProtocol,
    CheckingTunnelInterface,
    WaitingHandshake,
    ApplyingRoutes,
    CheckingFirewall,
    CheckingDns,
    CheckingTraffic,
    Connecting,
    Checking,
    Healthy,
    BackendFailed,
    LocalNetworkUnavailable,
    ApiUnavailable,
    ApiSslError,
    ApiDecryptionFailed,
    LocalServiceUnavailable,
    ProtocolStartFailed,
    TunnelInterfaceMissing,
    HandshakeTimeout,
    RouteMismatch,
    FirewallBlocked,
    DnsFailed,
    NoTraffic,
    Recovering,
    UnknownFailure
};
Q_ENUM_NS(ConnectionHealth)
}

using ConnectionHealth = connection_health_ns::ConnectionHealth;

QString connectionLifecycleText(Vpn::ConnectionState state);
QString connectionHealthText(ConnectionHealth health);
bool connectionHealthVisible(ConnectionHealth health);
bool connectionHealthProblem(ConnectionHealth health);
QString connectionStatusText(Vpn::ConnectionState state, ConnectionHealth health);
}

Q_DECLARE_METATYPE(amnezia::ConnectionHealth)

#endif // CONNECTIONHEALTH_H
