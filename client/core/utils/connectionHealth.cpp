#include "connectionHealth.h"

namespace amnezia
{
QString connectionLifecycleText(Vpn::ConnectionState state)
{
    switch (state) {
    case Vpn::ConnectionState::Disconnected:
        return QObject::tr("Connect");
    case Vpn::ConnectionState::Preparing:
        return QObject::tr("Preparing");
    case Vpn::ConnectionState::Connecting:
        return QObject::tr("Connecting");
    case Vpn::ConnectionState::Connected:
        return QObject::tr("Connected");
    case Vpn::ConnectionState::Reconnecting:
        return QObject::tr("Reconnecting");
    case Vpn::ConnectionState::Disconnecting:
        return QObject::tr("Disconnecting");
    case Vpn::ConnectionState::Error:
        return QObject::tr("Error");
    case Vpn::ConnectionState::Unknown:
    default:
        return QObject::tr("Checking");
    }
}

QString connectionHealthText(ConnectionHealth health)
{
    switch (health) {
    case ConnectionHealth::Idle:
        return QString();
    case ConnectionHealth::PreparingConfig:
        return QObject::tr("Preparing configuration");
    case ConnectionHealth::CheckingLocalNetwork:
        return QObject::tr("Checking internet connection");
    case ConnectionHealth::CheckingApi:
        return QObject::tr("Checking Amnezia API");
    case ConnectionHealth::CheckingLocalService:
        return QObject::tr("Checking local VPN service");
    case ConnectionHealth::StartingProtocol:
        return QObject::tr("Starting VPN protocol");
    case ConnectionHealth::CheckingTunnelInterface:
        return QObject::tr("Checking VPN tunnel interface");
    case ConnectionHealth::WaitingHandshake:
        return QObject::tr("Waiting for VPN server response");
    case ConnectionHealth::ApplyingRoutes:
        return QObject::tr("Applying network routes");
    case ConnectionHealth::CheckingFirewall:
        return QObject::tr("Checking firewall rules");
    case ConnectionHealth::CheckingDns:
        return QObject::tr("Checking DNS");
    case ConnectionHealth::CheckingTraffic:
    case ConnectionHealth::Checking:
        return QObject::tr("Checking traffic");
    case ConnectionHealth::Connecting:
        return QObject::tr("Connecting...");
    case ConnectionHealth::Healthy:
        return QObject::tr("VPN is working");
    case ConnectionHealth::BackendFailed:
    case ConnectionHealth::LocalServiceUnavailable:
        return QObject::tr("Local VPN service failed");
    case ConnectionHealth::LocalNetworkUnavailable:
        return QObject::tr("Internet connection is unavailable");
    case ConnectionHealth::ApiUnavailable:
        return QObject::tr("Amnezia API is not responding");
    case ConnectionHealth::ProtocolStartFailed:
        return QObject::tr("VPN protocol failed to start");
    case ConnectionHealth::TunnelInterfaceMissing:
        return QObject::tr("VPN tunnel interface is missing");
    case ConnectionHealth::HandshakeTimeout:
        return QObject::tr("VPN server is not responding");
    case ConnectionHealth::RouteMismatch:
        return QObject::tr("Network routes do not match VPN mode");
    case ConnectionHealth::FirewallBlocked:
        return QObject::tr("Firewall is blocking VPN traffic");
    case ConnectionHealth::DnsFailed:
        return QObject::tr("DNS is not resolving domains");
    case ConnectionHealth::NoTraffic:
        return QObject::tr("VPN is connected, but traffic is not passing");
    case ConnectionHealth::Recovering:
        return QObject::tr("Recovering connection...");
    case ConnectionHealth::UnknownFailure:
        return QObject::tr("Connection failed for an unknown reason");
    case ConnectionHealth::Unknown:
    default:
        return QObject::tr("Checking connection...");
    }
}

bool connectionHealthVisible(ConnectionHealth health)
{
    return health != ConnectionHealth::Idle;
}

bool connectionHealthProblem(ConnectionHealth health)
{
    switch (health) {
    case ConnectionHealth::BackendFailed:
    case ConnectionHealth::LocalNetworkUnavailable:
    case ConnectionHealth::ApiUnavailable:
    case ConnectionHealth::LocalServiceUnavailable:
    case ConnectionHealth::ProtocolStartFailed:
    case ConnectionHealth::TunnelInterfaceMissing:
    case ConnectionHealth::HandshakeTimeout:
    case ConnectionHealth::RouteMismatch:
    case ConnectionHealth::FirewallBlocked:
    case ConnectionHealth::DnsFailed:
    case ConnectionHealth::NoTraffic:
    case ConnectionHealth::UnknownFailure:
        return true;
    default:
        return false;
    }
}

QString connectionStatusText(Vpn::ConnectionState state, ConnectionHealth health)
{
    switch (state) {
    case Vpn::ConnectionState::Connecting:
    case Vpn::ConnectionState::Reconnecting:
    case Vpn::ConnectionState::Error:
        return connectionHealthText(health);
    case Vpn::ConnectionState::Disconnected:
    case Vpn::ConnectionState::Preparing:
    case Vpn::ConnectionState::Disconnecting:
    case Vpn::ConnectionState::Connected:
    case Vpn::ConnectionState::Unknown:
    default:
        return connectionLifecycleText(state);
    }
}
}
