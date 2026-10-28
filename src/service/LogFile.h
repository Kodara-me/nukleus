#pragma once

#include <QFile>
#include <QString>
#include <QtGlobal>

namespace NukleusService
{
// Appends messages to a file, one line each starting with an ISO timestamp and the level. When the file grows
// past `maxBytes` it is renamed to "<path>.1", replacing an earlier one, and a new file is started.
class LogFile
{
public:
    static constexpr qint64 DefaultMaxBytes = 10LL * 1024 * 1024;

    explicit LogFile(QString path, qint64 maxBytes = DefaultMaxBytes);

    // Returns false, with lastError() set, when the file cannot be written.
    bool write(QtMsgType type, const QString &message);
    QString lastError() const { return m_error; }
    QString path() const { return m_path; }

    // The level as written in the file: DEBUG, INFO, WARN, ERROR or FATAL.
    static QString levelName(QtMsgType type);

private:
    bool open();
    bool rotate();

    QString m_path;
    qint64 m_maxBytes;
    QFile m_file;
    QString m_error;
};

// Sends every Qt message (qInfo, qWarning, ...) to the file named by NUKLEUS_LOG_FILE, on every platform; the
// directory is created when missing. Does nothing and returns true when the variable is unset or empty. On
// failure it returns false with a message and leaves the messages on their current streams.
bool installLogFileFromEnvironment(QString *error);

// True once messages go to the log file.
bool logFileActive();

// Closes the log file and puts the default message handler back.
void uninstallLogFile();

// A message of the program's own: in the log file when it is active, otherwise on stdout (information) or
// stderr (anything else), as before the log file existed.
void report(QtMsgType type, const QString &message);
}
