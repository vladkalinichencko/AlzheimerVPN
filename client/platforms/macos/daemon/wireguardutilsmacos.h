/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WIREGUARDUTILSMACOS_H
#define WIREGUARDUTILSMACOS_H

#include <QObject>
#include <QProcess>

#include "daemon/wireguardutils.h"
#include "macosroutemonitor.h"
#include "macosfirewall.h"

class WireguardUtilsMacos final : public WireguardUtils {
  Q_OBJECT

 public:
  WireguardUtilsMacos(QObject* parent);
  ~WireguardUtilsMacos();

  bool interfaceExists() override {
    return m_tunnel.state() == QProcess::Running;
  }
  QString interfaceName() override { return m_ifname; }
  bool addInterface(const InterfaceConfig& config) override;
  bool deleteInterface() override;

  bool updatePeer(const InterfaceConfig& config) override;
  bool deletePeer(const InterfaceConfig& config) override;
  QList<PeerStatus> getPeerStatus() override;

  bool updateRoutePrefix(const IPAddress& prefix) override;
  bool deleteRoutePrefix(const IPAddress& prefix) override;

  bool addExclusionRoute(const IPAddress& prefix) override;
  bool deleteExclusionRoute(const IPAddress& prefix) override;

  bool excludeLocalNetworks(const QList<IPAddress>& lanAddressRanges) override;

  void applyFirewallRules(FirewallParams& params);

 signals:
  void backendFailure();

 private slots:
  void tunnelStdoutReady();
  void tunnelErrorOccurred(QProcess::ProcessError error);

 private:
  QString uapiCommand(const QString& command);
  static int uapiErrno(const QString& command);
  QString waitForTunnelName(const QString& filename);
  // Pick the userspace backend executable based on protocolName and whether
  // any AmneziaWG obfuscation parameters are actually configured. Falls back
  // to plain wireguard-go when running amneziawg-go would not produce any
  // obfuscation anyway — that case is observed to break post-handshake DATA
  // exchange against a vanilla wireguard server.
  static QString backendExecutableName(const InterfaceConfig& config);

  QString m_ifname;
  QProcess m_tunnel;
  MacosRouteMonitor* m_rtmonitor = nullptr;
};

#endif  // WIREGUARDUTILSMACOS_H
