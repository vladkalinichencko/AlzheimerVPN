#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>

#include "wireGuardProtocol.h"
#include "core/utils/networkUtilities.h"

#include "mozilla/localsocketcontroller.h"

WireguardProtocol::WireguardProtocol(amnezia::Proto proto, const QJsonObject &configuration, QObject *parent)
    : VpnProtocol(configuration, parent),
      m_proto(proto)
{
    m_impl.reset(new LocalSocketController());
    connect(m_impl.get(), &ControllerImpl::connected, this,
            [this](const QString &pubkey, const QDateTime &connectionTimestamp) {
                setConnectionState(Vpn::ConnectionState::Connected);
            });
    connect(m_impl.get(), &ControllerImpl::statusUpdated, this,
            [this](const QString& serverIpv4Gateway,
                   const QString& deviceIpv4Address, uint64_t txBytes,
                   uint64_t rxBytes) {
                const QString previousGateway = m_vpnGateway;
                const QString previousLocal = m_vpnLocalAddress;

                if (!serverIpv4Gateway.isEmpty()) {
                    m_vpnGateway = serverIpv4Gateway;
                }
                if (!deviceIpv4Address.isEmpty()) {
                    m_vpnLocalAddress = deviceIpv4Address;
                }

                if ((!m_vpnGateway.isEmpty() && m_vpnGateway != previousGateway) ||
                    (!m_vpnLocalAddress.isEmpty() && m_vpnLocalAddress != previousLocal)) {
                    emit tunnelAddressesUpdated(m_vpnGateway, m_vpnLocalAddress);
                }

                setBytesChanged(rxBytes, txBytes);
            });

    connect(m_impl.get(), &ControllerImpl::disconnected, this,
            [this]() { setConnectionState(Vpn::ConnectionState::Disconnected); });
    connect(m_impl.get(), &ControllerImpl::backendFailure, this,
            [this](ErrorCode error) {
                setLastError(error);
                emit protocolError(error);
            });
    m_impl->initialize(nullptr, nullptr);
}

WireguardProtocol::~WireguardProtocol()
{
    WireguardProtocol::stop();
    QThread::msleep(200);
}

void WireguardProtocol::stop()
{
    if (m_stopped) {
        return;
    }

    m_stopped = true;
    stopMzImpl();
}

bool WireguardProtocol::requestStatus()
{
    m_impl->checkStatus();
    return true;
}

ErrorCode WireguardProtocol::startMzImpl()
{
    const QString protocolName = ProtocolUtils::protoToString(m_proto);
    QJsonObject vpnConfigData = m_rawConfig.value(ProtocolUtils::key_proto_config_data(m_proto)).toObject();
    vpnConfigData[configKey::hostName] = NetworkUtilities::getIPAddress(vpnConfigData.value(configKey::hostName).toString());
    m_rawConfig.insert(ProtocolUtils::key_proto_config_data(m_proto), vpnConfigData);
    m_rawConfig[configKey::vpnProto] = protocolName;
    m_rawConfig[configKey::hostName] = NetworkUtilities::getIPAddress(m_rawConfig[configKey::hostName].toString());

    m_impl->activate(m_rawConfig);
    return ErrorCode::NoError;
}

ErrorCode WireguardProtocol::stopMzImpl()
{
    m_impl->deactivate();
    return ErrorCode::NoError;
}


ErrorCode WireguardProtocol::start()
{
    m_stopped = false;
    return startMzImpl();
}
