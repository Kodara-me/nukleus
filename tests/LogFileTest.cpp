#include "LogFile.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using NukleusService::LogFile;

class LogFileTest : public QObject
{
    Q_OBJECT

private slots:
    void unsetVariableLeavesMessagesOnTheirStreams_R11();
    void writesTimestampAndLevelOnEachLine_R11();
    void keepsAMultiLineMessageTogether_R11();
    void rotatesPastTenMiB_R11();
    void replacesAnEarlierRotation_R11();
    void createsTheDirectoryAndRoutesQtMessages_R11();
    void reportsAnUnusableFile_R11();

private:
    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    // Writes messages of about a kilobyte until the file is renamed and a new one started.
    static bool writeUntilRotated(LogFile &log, const QString &tag)
    {
        const QString filler(1000, QLatin1Char('x'));
        for (int i = 0; i < 20000; ++i) {
            const qint64 before = QFileInfo(log.path()).size();
            if (!log.write(QtInfoMsg, tag + filler)) return false;
            if (QFileInfo(log.path()).size() < before) return true;
        }
        return false;
    }
};

void LogFileTest::unsetVariableLeavesMessagesOnTheirStreams_R11()
{
    qunsetenv("NUKLEUS_LOG_FILE");
    QString error;
    QVERIFY(NukleusService::installLogFileFromEnvironment(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(!NukleusService::logFileActive());
}

void LogFileTest::writesTimestampAndLevelOnEachLine_R11()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nukleusd.log"));
    LogFile log(path);
    QVERIFY(log.write(QtInfoMsg, QStringLiteral("listening on 127.0.0.1:9384")));
    QVERIFY(log.write(QtWarningMsg, QStringLiteral("ffmpeg was not found")));
    QVERIFY(log.write(QtCriticalMsg, QStringLiteral("could not listen")));
    const QList<QByteArray> lines = read(path).split('\n');
    QCOMPARE(lines.size(), 4); // the last one is empty: each line ends with a newline
    const QRegularExpression format(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3} (INFO|WARN|ERROR) .+$"));
    for (int i = 0; i < 3; ++i) QVERIFY2(format.match(QString::fromUtf8(lines.at(i))).hasMatch(), lines.at(i).constData());
    QVERIFY(lines.at(0).endsWith(" INFO listening on 127.0.0.1:9384"));
    QVERIFY(lines.at(1).endsWith(" WARN ffmpeg was not found"));
    QVERIFY(lines.at(2).endsWith(" ERROR could not listen"));
}

void LogFileTest::keepsAMultiLineMessageTogether_R11()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nukleusd.log"));
    LogFile log(path);
    QVERIFY(log.write(QtInfoMsg, QStringLiteral("[Nukleus][sd-cli] Command:\nsd-cli --steps 20")));
    QVERIFY(read(path).contains("Command:\nsd-cli --steps 20\n"));
}

void LogFileTest::rotatesPastTenMiB_R11()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nukleusd.log"));
    LogFile log(path);
    QVERIFY2(writeUntilRotated(log, QStringLiteral("first ")), qPrintable(log.lastError()));
    QVERIFY(QFileInfo::exists(path + QStringLiteral(".1")));
    QVERIFY(QFileInfo(path + QStringLiteral(".1")).size() > LogFile::DefaultMaxBytes);
    // The new file starts empty and takes the next message.
    QCOMPARE(QFileInfo(path).size(), 0);
    QVERIFY(log.write(QtInfoMsg, QStringLiteral("after the rotation")));
    const QByteArray current = read(path);
    QVERIFY(current.contains(" INFO after the rotation\n"));
    QVERIFY(!current.contains("first "));
    QVERIFY(read(path + QStringLiteral(".1")).contains(" INFO first "));
}

void LogFileTest::replacesAnEarlierRotation_R11()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nukleusd.log"));
    LogFile log(path);
    QVERIFY2(writeUntilRotated(log, QStringLiteral("first ")), qPrintable(log.lastError()));
    QVERIFY2(writeUntilRotated(log, QStringLiteral("second ")), qPrintable(log.lastError()));
    const QByteArray rotated = read(path + QStringLiteral(".1"));
    QVERIFY(rotated.contains(" INFO second "));
    QVERIFY(!rotated.contains("first "));
    QVERIFY(!QFileInfo::exists(path + QStringLiteral(".2")));
}

void LogFileTest::createsTheDirectoryAndRoutesQtMessages_R11()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("logs/nested/nukleusd.log"));
    qputenv("NUKLEUS_LOG_FILE", QFile::encodeName(path));
    QString error;
    QVERIFY2(NukleusService::installLogFileFromEnvironment(&error), qPrintable(error));
    QVERIFY(NukleusService::logFileActive());
    qInfo().noquote() << QStringLiteral("[Nukleus Web] Listening on 127.0.0.1 port 9385.");
    NukleusService::report(QtCriticalMsg, QStringLiteral("nukleus-web: the port is in use"));
    const QByteArray content = read(path);
    QVERIFY2(content.contains(" INFO [Nukleus Web] Listening on 127.0.0.1 port 9385.\n"), content.constData());
    QVERIFY2(content.contains(" ERROR nukleus-web: the port is in use\n"), content.constData());
    NukleusService::uninstallLogFile();
    qunsetenv("NUKLEUS_LOG_FILE");
}

void LogFileTest::reportsAnUnusableFile_R11()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A directory where the file should be cannot be opened for writing.
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nukleusd.log")));
    LogFile log(dir.filePath(QStringLiteral("nukleusd.log")));
    QVERIFY(!log.write(QtInfoMsg, QStringLiteral("lost")));
    QVERIFY(!log.lastError().isEmpty());
}

QTEST_GUILESS_MAIN(LogFileTest)
#include "LogFileTest.moc"
