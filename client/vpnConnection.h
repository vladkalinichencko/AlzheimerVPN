#ifndef VPNCONNECTION_H
#define VPNCONNECTION_H

#include <QList>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QRemoteObjectNode>
#include <QScopedPointer>
#include <QString>
#include <QTimer>

#include "core/protocols/vpnProtocol.h"
#include "core/utils/connectionHealth.h"
#include "core/utils/errorCodes.h"
#include "core/utils/routeModes.h"
#include "core/utils/commonStructs.h"
#include "core/repositories/secureServersRepository.h"
#include "core/repositories/secureAppSettingsRepository.h"

#ifdef AMNEZIA_DESKTOP
#include "core/utils/ipcClient.h"
#endif

#ifdef Q_OS_ANDROID
#include "core/protocols/androidVpnProtocol.h"
#endif

using namespace amnezia;

class QNetworkReply;

class VpnConnection : public QObject
{
    Q_OBJECT

public:
    explicit VpnConnection(SecureServersRepository* serversRepository, SecureAppSettingsRepository* appSettingsRepository, QObject* parent = nullptr);
    ~VpnConnection() override;

    static QString bytesPerSecToText(quint64 bytes);

    ErrorCode lastError() const;
    Vpn::ConnectionState connectionState() const;
    ConnectionHealth connectionHealth() const;

    QSharedPointer<VpnProtocol> vpnProtocol() const;

    const QString &remoteAddress() const;
    void addSitesRoutes(const QString &gw, amnezia::RouteMode mode);

#ifdef Q_OS_ANDROID
    void restoreConnection();
#endif

public slots:
    void setRepositories(SecureServersRepository* serversRepository, SecureAppSettingsRepository* appSettingsRepository);
    void connectToVpn(int serverIndex, DockerContainer container, const QJsonObject &vpnConfiguration);
    void reconnectToVpn();
    void disconnectFromVpn();
    void refreshSiteSplitTunnelRoutes();

    void onKillSwitchModeChanged(bool enabled);
    void setConnectionDiagnostic(ConnectionHealth health);
    void disconnectSlots();

    void setConnectionState(Vpn::ConnectionState state);

signals:
    void bytesChanged(quint64 receivedBytes, quint64 sentBytes);
    void connectionStateChanged(Vpn::ConnectionState state);
    void connectionHealthChanged(ConnectionHealth health);
    void vpnProtocolError(amnezia::ErrorCode error);

    void serviceIsNotReady();

protected slots:
    void onBytesChanged(quint64 receivedBytes, quint64 sentBytes);
    void onConnectionStateChanged(Vpn::ConnectionState state);
    void onProtocolError(amnezia::ErrorCode error);
    void onConnectingTimedOut();
    void checkConnectedHealth();
    void onConnectivityProbeSucceeded();
    void onConnectivityProbeFailed();

protected:
    QSharedPointer<VpnProtocol> m_vpnProtocol;
    // Overridable so unit tests can suppress real network probes and drive
    // onConnectivityProbeSucceeded/Failed by hand.
    virtual void connectConnectivityProbe();

private:
    // HTTP probes validate traffic. DNS failures have a separate diagnostic
    // state, because an IP-only HTTP probe can still pass while domain lookup
    // is broken.
    struct ConnectivityProbe
    {
        QList<QPointer<QNetworkReply>> replies;
        int finishedCount = 0;
        bool decided = false;
    };

    SecureServersRepository* m_serversRepository;
    SecureAppSettingsRepository* m_appSettingsRepository;

    QJsonObject m_vpnConfiguration;
    QJsonObject m_routeMode;
    QString m_remoteAddress;

    // Only for iOS for now, check counters
    QTimer m_checkTimer;
    QTimer m_connectingTimer;
    QTimer m_healthTimer;
    // Single debounced flushDns after the per-site route batch completes — the
    // previous design called flushDns once per resolved site and produced
    // hundreds of "Failed to flush DNS" warnings on configs with large split
    // lists (and also slowed teardown by piling work on the daemon).
    QTimer m_dnsFlushDebounce;
    QScopedPointer<QNetworkAccessManager> m_probeNetworkManager;
    QScopedPointer<ConnectivityProbe> m_connectivityProbe;

#ifdef Q_OS_ANDROID
   AndroidVpnProtocol* androidVpnProtocol = nullptr;

   AndroidVpnProtocol* createDefaultAndroidVpnProtocol();
   void createAndroidConnections();
#endif

   Vpn::ConnectionState m_connectionState = Vpn::ConnectionState::Disconnected;
   ConnectionHealth m_connectionHealth = ConnectionHealth::Idle;
   ErrorCode m_lastError = ErrorCode::NoError;
   int m_healthChecksWithoutTraffic = 0;
   bool m_noTrafficRecoveryAttempted = false;
   bool m_silentReconnectInProgress = false;
   bool m_stoppingAfterFailure = false;
   int m_currentServerIndex = -1;
   DockerContainer m_currentContainer = DockerContainer::None;

   void createProtocolConnections();
   void setConnectionHealth(ConnectionHealth health);
   bool shouldPublishConnectionHealth(ConnectionHealth health) const;
   void startConnectingWatchdog();
   void stopConnectingWatchdog();
   void startConnectedHealthCheck();
   void stopConnectedHealthCheck();
   void checkDnsHealth();
   void startConnectivityProbe();
   void stopConnectivityProbe();
   void handleProbeReplyFinished(QNetworkReply *reply);
   void markLastError(ErrorCode error);

   void appendSplitTunnelingConfig();
   void appendKillSwitchConfig();
   void configureDnsSplitTunnel(const QString &gw, amnezia::RouteMode mode);
   // Asynchronous: invokes onDone(ok) on the main thread when the IPC replies
   // arrive. Synchronous waitForFinished was here previously and caused stack
   // overflow when called from inside per-site QHostInfo callbacks (each
   // waitForFinished spun a nested event loop that dispatched the next pending
   // callback on the same stack frame, recursing until SIGBUS).
   void addSplitTunnelRoutesAsync(const QString &gw, amnezia::RouteMode mode,
                                  const QStringList &ips,
                                  std::function<void(bool)> onDone);
};

#endif // VPNCONNECTION_H
