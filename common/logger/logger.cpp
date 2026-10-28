#include "logger.h"

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QStandardPaths>
#include <QUrl>
#include <QCoreApplication>

#include "core/utils/utilities.h"
#include "version.h"

#ifdef AMNEZIA_DESKTOP
    #include <core/utils/ipcClient.h>
#endif

#ifdef Q_OS_IOS
    #include "core/utils/swiftBridge.h"
#endif

QFile Logger::m_file;
QRecursiveMutex Logger::m_mutex;
QString Logger::m_basePath;
bool Logger::m_debugEnabled = true;
QString Logger::m_logFileName = QString("%1.log").arg(APPLICATION_NAME);
QString Logger::m_serviceLogFileName = QString("%1.log").arg(SERVICE_NAME);

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (msg.simplified().isEmpty()) {
        return;
    }

    // Skip annoying messages from Qt
    if (msg.contains("OpenType support missing for")) {
        return;
    }

    if (msg.startsWith("Unknown property") || msg.startsWith("Could not create pixmap")
        || msg.startsWith("Populating font") || msg.startsWith("stale focus object")) {
        return;
    }

    switch (type) {
    case QtDebugMsg: Logger::Instance().debug() << msg; break;
    case QtInfoMsg: Logger::Instance().info() << msg; break;
    case QtWarningMsg: Logger::Instance().warning() << msg; break;
    case QtCriticalMsg: Logger::Instance().error() << msg; break;
    case QtFatalMsg: {
        Logger::Instance().error() << msg;
    } // Brackets are needed to ensure the destructor of LogStreamer is called before abort()
        abort();
    }
}

Logger &Logger::Instance()
{
    static Logger s;
    return s;
}

bool Logger::rotateLogFile(const QDateTime &now)
{
    if (m_basePath.isEmpty()) {
        return false;
    }
    QString path = m_basePath;
#if defined(Q_OS_MACOS) && !defined(MACOS_NE)
    const QFileInfo base(m_basePath);
    const QString prefix = base.completeBaseName() + '-';
    path = base.absoluteDir().filePath(prefix + now.toString("yyyyMMdd-HH") + ".log");
#endif
    if (m_file.isOpen() && m_file.fileName() == path) {
        return true;
    }
    m_file.close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        return false;
    }
#if defined(Q_OS_MACOS) && !defined(MACOS_NE)
    const QString oldestHour = now.addSecs(-24 * 3600).toString("yyyyMMdd-HH");
    const QDir dir = base.absoluteDir();
    for (const QString &name : dir.entryList({prefix + "????????" "-??.log"}, QDir::Files)) {
        const QString hour = name.mid(prefix.size(), 11);
        if (QDateTime::fromString(hour, "yyyyMMdd-HH").isValid() && hour < oldestHour) {
            QFile::remove(dir.filePath(name));
        }
    }
#endif
    return true;
}

bool Logger::init(bool isServiceLogger, bool debugEnabled)
{
    QMutexLocker lock(&m_mutex);
    QString path = isServiceLogger ? systemLogDir() : userLogsDir();
    QString logFileName = isServiceLogger ? m_serviceLogFileName : m_logFileName;
    QDir appDir(path);
    if (!appDir.mkpath(path)) {
        return false;
    }

    m_basePath = appDir.filePath(logFileName);
    m_debugEnabled = debugEnabled;
    if (!rotateLogFile(QDateTime::currentDateTimeUtc())) {
        return false;
    }
    qInstallMessageHandler(messageHandler);
    qInfo() << "Log session started pid=" << QCoreApplication::applicationPid()
            << "debug=" << debugEnabled;
    return true;
}

void Logger::deInit()
{
    QMutexLocker lock(&m_mutex);
#if defined(Q_OS_MACOS) && !defined(MACOS_NE)
    m_debugEnabled = false;
#else
    m_basePath.clear();
    m_file.close();
#endif
}

bool Logger::setServiceLogsEnabled(bool enabled)
{
#ifdef AMNEZIA_DESKTOP
    return IpcClient::withInterface([enabled](QSharedPointer<IpcInterfaceReplica> iface) {
        iface->setLogsEnabled(enabled);
        qDebug() << "Logger::setServiceLogsEnabled(): Logs transitioned to be " << (enabled ? "enabled" : "disabled");
        return true;
    },[](){
        qWarning() << "Logger::setServiceLogsEnabled(): Service is not running";
        return false;
    });
#endif

    return true;
}

QString Logger::userLogsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/log";
}

QString Logger::systemLogDir()
{
#ifdef Q_OS_WIN
    QStringList locationList = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    QString primaryLocation = "ProgramData";
    foreach (const QString &location, locationList) {
        if (location.contains(primaryLocation)) {
            return QString("%1/%2/log").arg(location).arg(APPLICATION_NAME);
        }
    }
    return QString();
#else
    return QString("/var/log/%1").arg(APPLICATION_NAME);
#endif
}

QString Logger::userLogsFilePath()
{
    return userLogsDir() + QDir::separator() + m_logFileName;
}

QString Logger::serviceLogsFilePath()
{
    return systemLogDir() + QDir::separator() + m_serviceLogFileName;
}

QString Logger::getLogFile()
{
    const QString qtLog = readLogFiles(userLogsFilePath());

#ifdef Q_OS_IOS
    return QString().fromStdString(SWIFT_BRIDGE_NAMESPACE::swiftUpdateLogData(qtLog.toStdString()));
#else
    return qtLog;
#endif
}

QString Logger::getServiceLogFile()
{
    const QString qtLog = readLogFiles(serviceLogsFilePath());

#ifdef Q_OS_IOS
    return QString().fromStdString(SWIFT_BRIDGE_NAMESPACE::swiftUpdateLogData(qtLog.toStdString()));
#else
    return qtLog;
#endif
}

QString Logger::readLogFiles(const QString &basePath)
{
    QMutexLocker lock(&m_mutex);
    if (m_file.isOpen()) {
        m_file.flush();
    }
    QStringList paths {basePath};
#if defined(Q_OS_MACOS) && !defined(MACOS_NE)
    const QFileInfo base(basePath);
    const QDir dir = base.absoluteDir();
    paths.clear();
    const QString prefix = base.completeBaseName() + '-';
    const QString oldestHour = QDateTime::currentDateTimeUtc().addSecs(-24 * 3600).toString("yyyyMMdd-HH");
    for (const QString &name : dir.entryList({prefix + "????????" "-??.log"}, QDir::Files, QDir::Name)) {
        if (name.mid(prefix.size(), 11) >= oldestHour) {
            paths.append(dir.filePath(name));
        }
    }
#endif
    QString result;
    for (const QString &path : paths) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            result += QString::fromUtf8(file.readAll());
        }
    }
    return result;
}

bool Logger::openLogsFolder(bool isServiceLogger)
{
    QString path = isServiceLogger ? systemLogDir() : userLogsDir();
#ifdef Q_OS_WIN
    path = "file:///" + path;
#endif
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        qWarning() << "Can't open url:" << path;
        return false;
    }
    return true;
}

void Logger::clearLogs(bool isServiceLogger)
{
    QMutexLocker lock(&m_mutex);
    bool isLogActive = m_file.isOpen();
    const bool debugEnabled = m_debugEnabled;
    m_file.close();

#if defined(Q_OS_MACOS) && !defined(MACOS_NE)
    const QFileInfo base(isServiceLogger ? serviceLogsFilePath() : userLogsFilePath());
    const QDir dir = base.absoluteDir();
    for (const QString &name : dir.entryList({base.completeBaseName() + "-????????" "-??.log"}, QDir::Files)) {
        QFile::remove(dir.filePath(name));
    }
#endif

    QFile file(isServiceLogger ? serviceLogsFilePath() : userLogsFilePath());

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Can't clear log file:" << file.fileName();
        if (isLogActive) {
            init(isServiceLogger, debugEnabled);
        }
        return;
    }
    file.resize(0);
    file.close();

#ifdef Q_OS_IOS
    SWIFT_BRIDGE_NAMESPACE::swiftDeleteLog();
#endif

    if (isLogActive) {
        init(isServiceLogger, debugEnabled);
    }
}

void Logger::clearServiceLogs()
{
#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([](QSharedPointer<IpcInterfaceReplica> iface) {
        iface->clearLogs();
        qDebug() << "Logger::clearServiceLogs(): Logs cleared";
    }, []() {
        qWarning() << "Logger::clearServiceLogs(): Service is not running";
    });
#endif
}

void Logger::cleanUp()
{
    clearLogs(false);
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dir.removeRecursively();

    clearLogs(true);
}

Logger::LogStreamer::LogStreamer(Logger *logger, LogLevel logLevel)
    : m_logger(logger), m_logLevel(logLevel), m_data(new Data())
{
}

Logger::LogStreamer::~LogStreamer()
{
    QString logLevelString;
    switch (m_logLevel) {
    case LogLevel::Trace: logLevelString = "[TRACE]"; break;
    case LogLevel::Debug: logLevelString = "[DEBUG]"; break;
    case LogLevel::Info: logLevelString = "[INFO]"; break;
    case LogLevel::Warning: logLevelString = "[WARNING]"; break;
    case LogLevel::Error: logLevelString = "[ERROR]"; break;
    }

    const QString message = QString("%1 %2 %3 %4 : %5")
                                    .arg(QDateTime::currentDateTimeUtc().toString("[yyyy-MM-dd hh:mm:ss.zzzZ]"),
                                         logLevelString, APPLICATION_NAME, m_logger->className(), m_data->m_buffer.trimmed());

    QMutexLocker lock(&m_mutex);
    const bool saveMessage = m_debugEnabled || m_logLevel >= LogLevel::Info;
    if (saveMessage && !m_basePath.isEmpty() && rotateLogFile(QDateTime::currentDateTimeUtc())) {
        QTextStream logToFile(&m_file);
        logToFile << message << Qt::endl << Qt::flush;
    }

    QTextStream logToOutput((m_logLevel == LogLevel::Error) ? stderr : stdout);
    logToOutput << message << Qt::endl << Qt::flush;

    delete m_data;
}

Logger::LogStreamer Logger::error()
{
    return { this, LogLevel::Error };
}

Logger::LogStreamer Logger::warning()
{
    return { this, LogLevel::Warning };
}

Logger::LogStreamer Logger::info()
{
    return { this, LogLevel::Info };
}

Logger::LogStreamer Logger::debug()
{
    return { this, LogLevel::Debug };
}

QString Logger::sensitive(const QString &input)
{
#ifdef Q_DEBUG
    return input;
#else
    Q_UNUSED(input);
    return { 8, 'X' };
#endif
}

#define CREATE_LOGSTREAMER_OP_REF(x)                                                                                   \
    Logger::LogStreamer &Logger::LogStreamer::operator<<(x t)                                                          \
    {                                                                                                                  \
        m_data->m_ts << t << ' ';                                                                                      \
        return *this;                                                                                                  \
    }

CREATE_LOGSTREAMER_OP_REF(uint64_t);
CREATE_LOGSTREAMER_OP_REF(const char *);
CREATE_LOGSTREAMER_OP_REF(const QString &);
CREATE_LOGSTREAMER_OP_REF(const QByteArray &);
CREATE_LOGSTREAMER_OP_REF(const void *);

#undef CREATE_LOGSTREAMER_OP_REF

Logger::LogStreamer &Logger::LogStreamer::operator<<(const QStringList &t)
{
    m_data->m_ts << '[' << t.join(",") << ']' << ' ';
    return *this;
}

Logger::LogStreamer &Logger::LogStreamer::operator<<(const QJsonObject &t)
{
    m_data->m_ts << QJsonDocument(t).toJson(QJsonDocument::Indented) << ' ';
    return *this;
}

Logger::LogStreamer &Logger::LogStreamer::operator<<(QTextStreamFunction t)
{
    m_data->m_ts << t;
    return *this;
}

void Logger::LogStreamer::addMetaEnum(quint64 value, const QMetaObject *meta, const char *name)
{
    QMetaEnum me = meta->enumerator(meta->indexOfEnumerator(name));

    QString out;
    QTextStream ts(&out);

    if (const char *scope = me.scope()) {
        ts << scope << "::";
    }

    const char *key = me.valueToKey(static_cast<int>(value));
    const bool scoped = me.isScoped();
    if (scoped || !key) {
        ts << me.enumName() << (!key ? "(" : "::");
    }

    if (key) {
        ts << key;
    } else {
        ts << value << ")";
    }

    m_data->m_ts << out;
}
