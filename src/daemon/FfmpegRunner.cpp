#include "FfmpegRunner.h"

#include <algorithm>
#include <cmath>

#include <QDebug>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#endif

namespace
{
constexpr int ErrorTailCharacters = 4096;
constexpr int VersionTimeoutMs = 5000;
constexpr int TerminateGraceMs = 3000;

QString quotedArgument(QString argument)
{
    argument.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + argument + QLatin1Char('\'');
}

// The quality of the encoders without a compressionQuality: the value the fixed settings of earlier
// versions correspond to (WebP quality 90, MJPEG q:v 3, VP9 crf 24, H.264 crf 20).
constexpr int DefaultQuality = 90;

// Encoder arguments for the container an output file's extension names. quality (0-100, higher is better)
// maps linearly onto each encoder's own scale.
QStringList encoderArguments(const QString &output, int quality)
{
    const auto number = [](long value) { return QString::number(value); };
    const QString container = QFileInfo(output).suffix().toLower();
    if (container == QStringLiteral("webp"))
        return {QStringLiteral("-c:v"), QStringLiteral("libwebp_anim"), QStringLiteral("-quality"), number(quality),
                QStringLiteral("-loop"), QStringLiteral("0")};
    if (container == QStringLiteral("avi"))
        return {QStringLiteral("-c:v"), QStringLiteral("mjpeg"), QStringLiteral("-q:v"),
                number(std::clamp(std::lround(31 - quality * 0.311), 2L, 31L))};
    if (container == QStringLiteral("mp4"))
        return {QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-crf"), number(std::lround(40 - quality * 0.22)),
                QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-movflags"), QStringLiteral("+faststart")};
    return {QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"), QStringLiteral("-crf"), number(std::lround(63 - quality * 0.43)),
            QStringLiteral("-b:v"), QStringLiteral("0"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")};
}
}

FfmpegRunner::FfmpegRunner(QString executable, QObject *parent)
    : QObject(parent)
    , m_executable(std::move(executable))
{
    qRegisterMetaType<FfmpegResult>();
    m_process.setStandardOutputFile(QProcess::nullDevice());
#ifdef Q_OS_LINUX
    // A crashed daemon must not leave an encoder running.
    m_process.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_standardError = (m_standardError + QString::fromLocal8Bit(m_process.readAllStandardError())).right(ErrorTailCharacters);
    });
    connect(&m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && exitCode == 0, exitCode);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_active) return;
        m_standardError = (m_standardError + m_process.errorString()).right(ErrorTailCharacters);
        finish(false, -1);
    });
}

FfmpegRunner::~FfmpegRunner()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(TerminateGraceMs);
    }
}

QString FfmpegRunner::resolvedExecutable() const
{
    return m_executable.isEmpty() ? QString() : QStandardPaths::findExecutable(m_executable);
}

bool FfmpegRunner::isAvailable() const
{
    const QString path = resolvedExecutable();
    if (path.isEmpty()) return false;
    const QDateTime modified = QFileInfo(path).lastModified();
    if (path == m_probedPath && modified == m_probedModified) return m_probedAvailable;
    QProcess probe;
    probe.setStandardOutputFile(QProcess::nullDevice());
    probe.setStandardErrorFile(QProcess::nullDevice());
    probe.start(path, {QStringLiteral("-version")});
    const bool finished = probe.waitForStarted(VersionTimeoutMs) && probe.waitForFinished(VersionTimeoutMs);
    if (!finished) {
        probe.kill();
        probe.waitForFinished(TerminateGraceMs);
    }
    m_probedPath = path;
    m_probedModified = modified;
    m_probedAvailable = finished && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    if (!m_probedAvailable) qWarning().noquote() << QStringLiteral("[Nukleus][ffmpeg] %1 does not run; video post-processing is unavailable").arg(path);
    return m_probedAvailable;
}

bool FfmpegRunner::isRunning() const
{
    return m_active;
}

QString FfmpegRunner::commandLine(const QStringList &arguments) const
{
    const QString path = resolvedExecutable();
    QStringList parts{quotedArgument(path.isEmpty() ? m_executable : path)};
    for (const auto &argument : arguments) parts.append(quotedArgument(argument));
    return parts.join(QLatin1Char(' '));
}

bool FfmpegRunner::run(const QStringList &arguments, const QString &outputPath)
{
    if (m_active) return false;
    m_active = true;
    m_arguments = arguments;
    m_outputPath = outputPath;
    m_standardError.clear();
    qInfo().noquote() << QStringLiteral("[Nukleus][ffmpeg] Command:\n%1").arg(commandLine(arguments));
    const QString path = resolvedExecutable();
    m_process.start(path.isEmpty() ? m_executable : path, arguments);
    return true;
}

void FfmpegRunner::cancel()
{
    if (!m_active || m_process.state() == QProcess::NotRunning) return;
    m_process.terminate();
    QTimer::singleShot(TerminateGraceMs, &m_process, [this] {
        if (m_process.state() != QProcess::NotRunning) m_process.kill();
    });
}

void FfmpegRunner::finish(bool succeeded, int exitCode)
{
    if (!m_active) return;
    m_active = false;
    m_standardError += QString::fromLocal8Bit(m_process.readAllStandardError());
    const FfmpegResult result{succeeded, exitCode, m_arguments, m_standardError.right(ErrorTailCharacters), m_outputPath};
    qInfo().noquote() << QStringLiteral("[Nukleus][ffmpeg] Finished with exit code %1").arg(exitCode);
    m_arguments.clear();
    m_outputPath.clear();
    emit finished(result);
}

QStringList FfmpegRunner::encodeArguments(const QString &input, const QString &output, int fps, bool loop,
                                          std::optional<int> quality)
{
    QStringList arguments{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
                          QStringLiteral("-y"), QStringLiteral("-i"), input};
    if (loop) {
        // The backward copy drops the first frame before reversing (so it does not end on it) and the last
        // frame after (so the turning frame is not repeated); concat then plays both copies in order.
        const QString graph = QStringLiteral(
            "[0:v]split[forward][source];"
            "[source]trim=start_frame=1,setpts=PTS-STARTPTS,reverse,trim=start_frame=1,setpts=PTS-STARTPTS[backward];"
            "[forward][backward]concat=n=2:v=1:a=0[loop]");
        arguments += {QStringLiteral("-filter_complex"), graph, QStringLiteral("-map"), QStringLiteral("[loop]")};
    } else {
        arguments += {QStringLiteral("-map"), QStringLiteral("0:v:0")};
    }
    arguments += {QStringLiteral("-an"), QStringLiteral("-r"), QString::number(fps)};
    return arguments + encoderArguments(output, quality.value_or(DefaultQuality)) + QStringList{output};
}
