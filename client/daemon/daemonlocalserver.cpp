/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "daemonlocalserver.h"

#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>

#include "daemonlocalserverconnection.h"
#include "leakdetector.h"
#include "logger.h"

#if defined(MZ_MACOS) || defined(MZ_LINUX)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#ifndef AMNEZIA_DAEMON_RUN_DIR
#define AMNEZIA_DAEMON_RUN_DIR "/var/run/amneziavpn"
#endif

#ifndef AMNEZIA_DAEMON_TMP_SOCKET
#define AMNEZIA_DAEMON_TMP_SOCKET "/tmp/amneziavpn.socket"
#endif

constexpr const char* TMP_PATH = AMNEZIA_DAEMON_TMP_SOCKET;
constexpr const char* RUN_DIR = AMNEZIA_DAEMON_RUN_DIR;
constexpr const char* DAEMON_SOCKET_NAME = "daemon.socket";
#endif

namespace {
Logger logger("DaemonLocalServer");
}  // namespace

DaemonLocalServer::DaemonLocalServer(QObject* parent) : QObject(parent) {
  MZ_COUNT_CTOR(DaemonLocalServer);
}

DaemonLocalServer::~DaemonLocalServer() { MZ_COUNT_DTOR(DaemonLocalServer); }

bool DaemonLocalServer::initialize() {
  m_server.setSocketOptions(QLocalServer::WorldAccessOption);

  QString path = daemonPath();
  logger.debug() << "Server path:" << path;

  if (QFileInfo::exists(path)) {
    QFile::remove(path);
  }

  if (!m_server.listen(path)) {
    logger.error() << "Failed to listen the daemon path";
    return false;
  }

  connect(&m_server, &QLocalServer::newConnection, [&] {
    logger.debug() << "New connection received";

    if (!m_server.hasPendingConnections()) {
      return;
    }

    QLocalSocket* socket = m_server.nextPendingConnection();
    Q_ASSERT(socket);

    DaemonLocalServerConnection* connection =
        new DaemonLocalServerConnection(&m_server, socket);
    connect(socket, &QLocalSocket::disconnected, connection,
            &DaemonLocalServerConnection::deleteLater);
  });

  return true;
}

QString DaemonLocalServer::daemonPath() const {
#if defined(MZ_WINDOWS)
  return "\\\\.\\pipe\\amneziavpn";
#endif
#if defined(MZ_MACOS) || defined(MZ_LINUX)
  QDir dir(RUN_DIR);
  if (dir.exists()) {
    logger.debug() << RUN_DIR << "seems to be usable";
    return dir.filePath(DAEMON_SOCKET_NAME);
  }

  if (!dir.mkpath(QStringLiteral("."))) {
    logger.warning() << "Failed to create" << RUN_DIR << "Fallback /tmp.";
    return TMP_PATH;
  }

  if (chmod(RUN_DIR, S_IRWXU | S_IRWXG | S_IRWXO) < 0) {
    logger.warning()
        << "Failed to set the right permissions to" << RUN_DIR;
    return TMP_PATH;
  }

  return dir.filePath(DAEMON_SOCKET_NAME);
#endif
}
