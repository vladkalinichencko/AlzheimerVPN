#include "ipcserverprocess.h"
#include "ipc.h"
#include <QProcess>

#ifndef Q_OS_IOS

IpcServerProcess::IpcServerProcess(QObject *parent) :
    IpcProcessInterfaceSource(parent),
    m_process(QSharedPointer<QProcess>(new QProcess()))
{
    connect(m_process.data(), &QProcess::errorOccurred, this, &IpcServerProcess::errorOccurred);
    connect(m_process.data(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &IpcServerProcess::finished);
    connect(m_process.data(), &QProcess::readyRead, this, &IpcServerProcess::readyRead);
    connect(m_process.data(), &QProcess::readyReadStandardError, this, [this]() {
        const auto previousChannel = m_process->readChannel();
        m_process->setReadChannel(QProcess::StandardError);
        const QByteArray chunk = m_process->peek(m_process->bytesAvailable());
        m_process->setReadChannel(previousChannel);
        appendProcessOutput(m_stderrTail, chunk);
        emit readyReadStandardError();
    });
    connect(m_process.data(), &QProcess::readyReadStandardOutput, this, [this]() {
        const auto previousChannel = m_process->readChannel();
        m_process->setReadChannel(QProcess::StandardOutput);
        const QByteArray chunk = m_process->peek(m_process->bytesAvailable());
        m_process->setReadChannel(previousChannel);
        appendProcessOutput(m_stdoutTail, chunk);
        emit readyReadStandardOutput();
    });
    connect(m_process.data(), &QProcess::started, this, &IpcServerProcess::started);
    connect(m_process.data(), &QProcess::stateChanged, this, &IpcServerProcess::stateChanged);

    connect(m_process.data(), &QProcess::errorOccurred, [this](QProcess::ProcessError error){
        logProcessSnapshot(QStringLiteral("errorOccurred"), error);
    });
    connect(m_process.data(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        qWarning().noquote()
            << "IpcServerProcess finished"
            << "program=" + m_process->program()
            << "exit_code=" + QString::number(exitCode)
            << "exit_status=" + QString::number(static_cast<int>(exitStatus))
            << "stderr_tail=" + QString::fromUtf8(m_stderrTail).simplified()
            << "stdout_tail=" + QString::fromUtf8(m_stdoutTail).simplified();
    });

}

void IpcServerProcess::appendProcessOutput(QByteArray &buffer, const QByteArray &chunk)
{
    buffer.append(chunk);
    constexpr qsizetype maxTailBytes = 4096;
    if (buffer.size() > maxTailBytes) {
        buffer = buffer.right(maxTailBytes);
    }
}

void IpcServerProcess::logProcessSnapshot(const QString &event, QProcess::ProcessError error)
{
    qWarning().noquote()
        << "IpcServerProcess" << event
        << "program=" + m_process->program()
        << "error=" + QString::number(static_cast<int>(error))
        << "state=" + QString::number(static_cast<int>(m_process->state()))
        << "stderr_tail=" + QString::fromUtf8(m_stderrTail).simplified()
        << "stdout_tail=" + QString::fromUtf8(m_stdoutTail).simplified();
}

IpcServerProcess::~IpcServerProcess()
{
    qDebug() << "IpcServerProcess::~IpcServerProcess";
}

void IpcServerProcess::start()
{
    if (m_process->program().isEmpty()) {
        qDebug() << "IpcServerProcess failed to start, program is empty";
    }

    Utils::killProcessByName(m_process->program());
    m_process->start();
    qDebug() << "IpcServerProcess started, " << m_process->program() << m_process->arguments();

    m_process->waitForStarted();
}

void IpcServerProcess::terminate() {
    m_process->terminate();
}

void IpcServerProcess::kill() {
    m_process->kill();
}

void IpcServerProcess::close()
{
    m_process->close();
}

void IpcServerProcess::setArguments(const QStringList &arguments)
{
    m_process->setArguments(amnezia::sanitizeArguments(m_program, arguments));
}

void IpcServerProcess::setInputChannelMode(QProcess::InputChannelMode mode)
{
     m_process->setInputChannelMode(mode);
}

void IpcServerProcess::setNativeArguments(const QString &arguments)
{
#ifdef Q_OS_WIN
    m_process->setNativeArguments(arguments);
#endif
}

void IpcServerProcess::setProcessChannelMode(QProcess::ProcessChannelMode mode)
{
    m_process->setProcessChannelMode(mode);
}

void IpcServerProcess::setProgram(int programId)
{
    m_program = static_cast<amnezia::PermittedProcess>(programId);
    m_process->setProgram(amnezia::permittedProcessPath(m_program));
    m_process->setArguments({});
}

void IpcServerProcess::setWorkingDirectory(const QString &dir)
{
    m_process->setWorkingDirectory(dir);
}

QByteArray IpcServerProcess::readAll()
{
    return m_process->readAll();
}

QByteArray IpcServerProcess::readAllStandardError()
{
    return m_process->readAllStandardError();
}

QByteArray IpcServerProcess::readAllStandardOutput()
{
    return m_process->readAllStandardOutput();
}

bool IpcServerProcess::waitForStarted() {
    return m_process->waitForStarted();
}

bool IpcServerProcess::waitForStarted(int msecs) {
    return m_process->waitForStarted(msecs);
}

bool IpcServerProcess::waitForFinished() {
    return m_process->waitForFinished();
}

bool IpcServerProcess::waitForFinished(int msecs) {
    return m_process->waitForFinished(msecs);
}

#endif
