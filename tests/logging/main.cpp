#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <thread>
#include <vector>
#include <cstdlib>
#include "logger.h"

void messageHandler(QtMsgType, const QMessageLogContext &, const QString &);

class LoggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString basePath;
public:
    static int writeInChild(const QString &path, const QString &message) {
        Logger::m_basePath = path;
        Logger::m_debugEnabled = false;
        Logger::Instance().info() << message;
        return Logger::m_file.isOpen() ? 0 : 1;
    }
private slots:
    void init() {
        QVERIFY(dir.isValid());
        basePath = dir.filePath(QUuid::createUuid().toString(QUuid::Id128) + ".log");
        Logger::m_basePath = basePath;
        Logger::m_debugEnabled = false;
        Logger::m_file.close();
    }
    void cleanup() {
        Logger::m_file.close();
        Logger::m_basePath.clear();
    }
    void survivesProcessRestart() {
        for (const QString &message : {QString("first-session"), QString("second-session")}) {
            QProcess process;
            process.start(QCoreApplication::applicationFilePath(), {"--write-log", basePath, message});
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitCode(), 0);
        }
        const QString text = Logger::readLogFiles(basePath);
        QVERIFY(text.contains("first-session"));
        QVERIFY(text.contains("second-session"));
    }
    void diagnosticsSurviveDebugDisable() {
        Logger::m_debugEnabled = true;
        Logger::Instance().debug() << "debug-before-disable";
        Logger::deInit();
        Logger::Instance().debug() << "debug-after-disable";
        Logger::Instance().info() << "connection-after-disable";
        Logger::Instance().warning() << "failure-after-disable";
        const QString text = Logger::readLogFiles(basePath);
        QVERIFY(text.contains("debug-before-disable"));
        QVERIFY(!text.contains("debug-after-disable"));
        QVERIFY(text.contains("connection-after-disable"));
        QVERIFY(text.contains("failure-after-disable"));
    }
    void capturesQtMessagesWhileLocked() {
        QVERIFY(Logger::rotateLogFile(QDateTime::currentDateTimeUtc()));
        const auto previousHandler = qInstallMessageHandler(messageHandler);
        {
            QMutexLocker lock(&Logger::m_mutex);
            qInfo("qt-connection-event");
        }
        qInstallMessageHandler(previousHandler);
        QVERIFY(Logger::readLogFiles(basePath).contains("qt-connection-event"));
    }
    void expiresOnlyOldHours() {
        const auto now = QDateTime::currentDateTimeUtc();
        const QString prefix = QFileInfo(basePath).completeBaseName() + '-';
        QStringList paths;
        for (int hours : {25, 24, 1}) {
            const QString path = dir.filePath(prefix + now.addSecs(-hours * 3600).toString("yyyyMMdd-HH") + ".log");
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("retained entry\n");
            paths.append(path);
        }
        QVERIFY(Logger::rotateLogFile(now));
        QVERIFY(!QFile::exists(paths[0]));
        QVERIFY(QFile::exists(paths[1]));
        QVERIFY(QFile::exists(paths[2]));
    }
    void rotatesAfterLongSleep() {
        const auto now = QDateTime::currentDateTimeUtc();
        QVERIFY(Logger::rotateLogFile(now));
        const QString oldPath = Logger::m_file.fileName();
        QVERIFY(Logger::rotateLogFile(now.addSecs(26 * 3600)));
        QVERIFY(Logger::m_file.fileName() != oldPath);
        QVERIFY(!QFile::exists(oldPath));
        QVERIFY(Logger::m_file.isOpen());
    }
    void serializesThreadedWrites() {
        std::vector<std::thread> threads;
        for (int thread = 0; thread < 4; ++thread) {
            threads.emplace_back([]() {
                for (int entry = 0; entry < 25; ++entry) {
                    Logger::Instance().info() << "threaded-entry";
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        QCOMPARE(Logger::readLogFiles(basePath).count("threaded-entry"), 100);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--write-log") {
        std::_Exit(LoggerTest::writeInChild(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3])));
    }
    LoggerTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "main.moc"
