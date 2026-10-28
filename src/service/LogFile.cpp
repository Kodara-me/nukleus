#include "LogFile.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <cstdio>
#include <memory>

namespace NukleusService
{
LogFile::LogFile(QString path, qint64 maxBytes)
    : m_path(std::move(path))
    , m_maxBytes(maxBytes)
    , m_file(m_path)
{
}

QString LogFile::levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return QStringLiteral("DEBUG");
    case QtInfoMsg: return QStringLiteral("INFO");
    case QtWarningMsg: return QStringLiteral("WARN");
    case QtCriticalMsg: return QStringLiteral("ERROR");
    case QtFatalMsg: return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

bool LogFile::open()
{
    if (m_file.isOpen()) return true;
    // Not in text mode: the lines end with a plain newline on every platform.
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
        m_error = m_file.errorString();
        return false;
    }
    return true;
}

bool LogFile::rotate()
{
    m_file.close();
    const QString previous = m_path + QStringLiteral(".1");
    QFile::remove(previous);
    if (!QFile::rename(m_path, previous)) {
        m_error = QStringLiteral("could not rename %1 to %2").arg(m_path, previous);
        return false;
    }
    return open();
}

bool LogFile::write(QtMsgType type, const QString &message)
{
    if (!open()) return false;
    const QString line = QStringLiteral("%1 %2 %3\n")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), levelName(type), message);
    const QByteArray bytes = line.toUtf8();
    if (m_file.write(bytes) != bytes.size()) {
        m_error = m_file.errorString();
        return false;
    }
    if (m_file.size() > m_maxBytes) return rotate();
    return true;
}

namespace
{
QMutex g_mutex;
std::unique_ptr<LogFile> g_file;

void handleMessage(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    {
        QMutexLocker locker(&g_mutex);
        if (g_file && g_file->write(type, message)) return;
    }
    // The file cannot be written: the message must not be lost.
    std::fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
}
}

bool installLogFileFromEnvironment(QString *error)
{
    const QString path = qEnvironmentVariable("NUKLEUS_LOG_FILE");
    if (path.isEmpty()) return true;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QDir().mkpath(QFileInfo(absolute).absolutePath());
    auto file = std::make_unique<LogFile>(absolute);
    // Opens the file now, so that an unusable path is reported at start and not on the first message.
    QFile probe(absolute);
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("Could not open the log file %1: %2").arg(absolute, probe.errorString());
        return false;
    }
    probe.close();
    {
        QMutexLocker locker(&g_mutex);
        g_file = std::move(file);
    }
    qInstallMessageHandler(handleMessage);
    return true;
}

bool logFileActive()
{
    QMutexLocker locker(&g_mutex);
    return g_file != nullptr;
}

void uninstallLogFile()
{
    qInstallMessageHandler(nullptr);
    QMutexLocker locker(&g_mutex);
    g_file.reset();
}

void report(QtMsgType type, const QString &message)
{
    if (logFileActive()) {
        switch (type) {
        case QtInfoMsg: qInfo().noquote() << message; break;
        case QtWarningMsg: qWarning().noquote() << message; break;
        default: qCritical().noquote() << message; break;
        }
        return;
    }
    QTextStream(type == QtInfoMsg ? stdout : stderr) << message << '\n';
}
}
