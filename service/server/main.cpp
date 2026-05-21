#include <QDir>

#include "version.h"
#include "localserver.h"
#include "logger.h"
#include "systemservice.h"
#include "core/utils/utilities.h"

#ifdef Q_OS_WIN
#include "platforms/windows/daemon/windowsdaemontunnel.h"

namespace {
int s_argc = 0;
char** s_argv = nullptr;
}  // namespace

#endif

int runApplication(int argc, char** argv)
{
    QCoreApplication app(argc,argv);

    // Always init service-side logger at startup. Upstream Amnezia only inits
    // the file logger after the GUI sends an IPC `setLogsEnabled(true)` — that
    // is useless for debugging daemon-startup failures: if the daemon dies or
    // zombifies (e.g. QLocalServer::listen() fails and we get the silent
    // headless-process state captured at 2026-05-21 18:13) the GUI can never
    // send the toggle and the daemon never writes a single line. With this
    // unconditional init, /var/log/<APP>/<SERVICE>.log gets the daemon-side
    // story from the moment the process starts, regardless of GUI state.
    if (Logger::init(true)) {
        qInfo() << "Daemon log file initialized at" << Logger::serviceLogsFilePath();
    }

#ifdef Q_OS_WIN
    if(argc > 2){
        s_argc = argc;
        s_argv = argv;
        QStringList tokens;
        for (int i = 1; i < argc; ++i) {
            tokens.append(QString(argv[i]));
        }

        if (!tokens.empty() && tokens[0] == "tunneldaemon") {
            WindowsDaemonTunnel *daemon = new WindowsDaemonTunnel();
            daemon->run(tokens);
        }
    }
#endif

    LocalServer localServer;
    return app.exec();

}


int main(int argc, char **argv)
{
    Utils::initializePath(Logger::systemLogDir());

    if (argc >= 2) {
        qInfo() << "Started as console application";
        return runApplication(argc, argv);
    }
    else {
        qInfo() << "Started as system service";
#ifdef Q_OS_WIN
        SystemService systemService(argc, argv);
        return systemService.exec();
#else
    return runApplication(argc, argv);
#endif

    }
}
