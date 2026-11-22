/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "macosdaemon.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QtGlobal>

#include "leakdetector.h"
#include "logger.h"
#include "core/utils/connectivityProbeTargets.h"
#include "version.h"

namespace {
Logger logger("MacOSDaemon");
MacOSDaemon* s_daemon = nullptr;

void traceCommand(QObject* parent, const QString& label, const QString& program,
                  const QStringList& args, const QRegularExpression& fields) {
  auto* process = new QProcess(parent);
  QObject::connect(process, &QProcess::finished, process,
                   [process, label, fields](int exitCode, QProcess::ExitStatus status) {
    QStringList values;
    const QString output = QString::fromUtf8(process->readAllStandardOutput());
    auto matches = fields.globalMatch(output);
    while (matches.hasNext()) {
      values.append(matches.next().captured().simplified());
    }
    logger.info() << label << "exit=" << (status == QProcess::NormalExit ? QString::number(exitCode) : QStringLiteral("crash"))
                  << "fields=" << values.join(QStringLiteral(" | "));
    process->deleteLater();
  });
  QObject::connect(process, &QProcess::errorOccurred, process, [process, label](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      logger.warning() << label << "command failed to start";
      process->deleteLater();
    }
  });
  QTimer::singleShot(2000, process, [process, label] {
    if (process->state() != QProcess::NotRunning) {
      logger.warning() << label << "command timed out";
      process->kill();
    }
  });
  process->start(program, args, QIODevice::ReadOnly);
}
}  // namespace

MacOSDaemon::MacOSDaemon() : Daemon(nullptr) {
  MZ_COUNT_CTOR(MacOSDaemon);

  logger.debug() << "Daemon created";

  m_wgutils = new WireguardUtilsMacos(this);
  m_dnsutils = new DnsUtilsMacos(this);
  m_iputils = new IPUtilsMacos(this);
  connect(m_dnsutils, &DnsUtils::splitTunnelHostResolved,
          this, &MacOSDaemon::onSplitTunnelHostResolved);
  connect(m_wgutils, &WireguardUtilsMacos::backendFailure, this, [this] {
    if (!m_connections.isEmpty() && !m_wgutils->interfaceExists()) {
      deactivate(false);
      emit backendFailure();
    }
  }, Qt::QueuedConnection);
  connect(this, &Daemon::connected, this, [this] {
    const auto attempt = m_traceAttempt;
    for (const int delay : {0, 7000, 23000}) {
      QTimer::singleShot(delay, this, [this, attempt, delay] {
        if (m_traceAttempt != attempt || !m_traceClock.isValid() || m_connections.isEmpty()) {
          return;
        }
        traceNetwork(QStringLiteral("handshake+%1ms").arg(delay), m_connections.first().m_config);
      });
    }
  });

  Q_ASSERT(s_daemon == nullptr);
  s_daemon = this;
}

MacOSDaemon::~MacOSDaemon() {
  MZ_COUNT_DTOR(MacOSDaemon);

  logger.debug() << "Daemon released";

  Q_ASSERT(s_daemon == this);
  s_daemon = nullptr;
}

// static
MacOSDaemon* MacOSDaemon::instance() {
  Q_ASSERT(s_daemon);
  return s_daemon;
}

bool MacOSDaemon::run(Op op, const InterfaceConfig& config) {
  if (op == Down) {
    logger.info() << "NetTrace teardown attempt=" << m_traceAttempt
                  << "elapsed_ms=" << QString::number(m_traceClock.isValid() ? m_traceClock.elapsed() : -1);
    m_traceClock.invalidate();
    return true;
  }

  ++m_traceAttempt;
  m_traceClock.start();
  logger.info() << "NetTrace activation attempt=" << m_traceAttempt << "protocol=" << config.m_protocolName
                << "tunnel=" << m_wgutils->interfaceName() << "mtu=" << QString::number(config.m_deviceMTU)
                << "kill_switch=" << QString::number(config.m_killSwitchEnabled)
                << "tunnel_prefixes=" << QString::number(config.m_allowedIPAddressRanges.size())
                << "bypass_prefixes=" << QString::number(config.m_excludedAddresses.size())
                << "host_rules=" << QString::number(config.m_splitTunnelDnsRules.size());
  traceNetwork(QStringLiteral("routes-installed"), config);
  return true;
}

QJsonObject MacOSDaemon::getStatus() {
  const auto attempt = m_traceAttempt;
  const auto status = Daemon::getStatus();
  if (attempt == m_traceAttempt && m_traceClock.isValid() && m_traceClock.elapsed() < 45000) {
    logger.info() << "NetTrace traffic attempt=" << attempt << "elapsed_ms=" << QString::number(m_traceClock.elapsed())
                  << "peer_found=" << QString::number(status.value("connected").toBool())
                  << "rx_bytes=" << QString::number(status.value("rxBytes").toInteger(-1))
                  << "tx_bytes=" << QString::number(status.value("txBytes").toInteger(-1));
  }
  return status;
}

void MacOSDaemon::traceNetwork(const QString& phase, const InterfaceConfig& config) {
  const QString label = QStringLiteral("NetTrace attempt=%1 phase=%2 elapsed_ms=%3 ")
                            .arg(m_traceAttempt).arg(phase).arg(m_traceClock.elapsed());
  static const QRegularExpression routeFields(
      QStringLiteral(R"(^\h*(?:interface:\h*[a-zA-Z0-9]+|flags:\h*<[A-Z0-9_,]+>))"),
      QRegularExpression::MultilineOption);
  auto traceRoute = [&](const QString& role, const QString& host) {
    const QHostAddress address(host);
    if (address.isNull()) {
      return;
    }
    traceCommand(this, label + QStringLiteral("route=") + role, QStringLiteral("/sbin/route"),
                 {QStringLiteral("-n"), QStringLiteral("get"),
                  address.protocol() == QAbstractSocket::IPv6Protocol ? QStringLiteral("-inet6") : QStringLiteral("-inet"),
                  address.toString()}, routeFields);
  };
  traceRoute(QStringLiteral("endpoint"), config.m_serverIpv4AddrIn);
  traceRoute(QStringLiteral("dns-primary"), config.m_primaryDnsServer);
  traceRoute(QStringLiteral("dns-secondary"), config.m_secondaryDnsServer);
  for (qsizetype i = 0; i < kConnectivityProbeHosts.size(); ++i) {
    traceRoute(QStringLiteral("tcp-probe-%1").arg(i + 1), kConnectivityProbeHosts[i]);
  }

  static const QRegularExpression dnsFields(
      QStringLiteral(R"(^\h*(?:resolver #\d+|if_index\h*:\h*\d+(?:\h*\([a-zA-Z0-9]+\))?|nameserver\[\d+\](?:\h*:\h*127\.0\.0\.1\b)?))"),
      QRegularExpression::MultilineOption);
  traceCommand(this, label + QStringLiteral("system-dns"), QStringLiteral("/usr/sbin/scutil"),
               {QStringLiteral("--dns")}, dnsFields);

  static const QRegularExpression pfFields(
      QStringLiteral(R"(^\h*(?:@\d+\h+(?:pass|block)(?: return)?\b|\[\h*Evaluations:\h*\d+\h+Packets:\h*\d+\h+Bytes:\h*\d+))"),
      QRegularExpression::MultilineOption);
  static const QRegularExpression pfRootFields(
      QStringLiteral(R"(^\h*(?:@\d+\h+)?anchor\h+"(?:alzheimervpn|amn)(?:/\*)?")"),
      QRegularExpression::MultilineOption);
  traceCommand(this, label + QStringLiteral("pf-root"), QStringLiteral("/sbin/pfctl"),
               {QStringLiteral("-vvs"), QStringLiteral("rules")}, pfRootFields);
  const QString anchor = QStringLiteral(SERVICE_NAME) == QStringLiteral("AmneziaVPN-service")
      ? QStringLiteral("amn/") : QStringLiteral("alzheimervpn/");
  for (const QString& rule : {QStringLiteral("100.blockAll"), QStringLiteral("110.allowNets"),
                              QStringLiteral("120.blockNets"), QStringLiteral("200.allowVPN"),
                              QStringLiteral("310.blockDNS")}) {
    traceCommand(this, label + QStringLiteral("pf=") + rule, QStringLiteral("/sbin/pfctl"),
                 {QStringLiteral("-a"), anchor + rule, QStringLiteral("-vvs"), QStringLiteral("rules")}, pfFields);
  }
}
