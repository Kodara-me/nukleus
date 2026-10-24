#pragma once

#include <QDateTime>
#include <QMetaType>
#include <optional>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

struct FfmpegResult
{
    bool succeeded = false;
    int exitCode = -1;
    QStringList arguments;
    QString standardError; // Its trailing 4096 characters.
    QString outputPath;
};

Q_DECLARE_METATYPE(FfmpegResult)

// ffmpeg, the optional runtime tool that post-processes video jobs (ping-pong loops, MP4 encoding): found on
// PATH or named by NUKLEUS_FFMPEG, and run one at a time with an argument list, never through a shell.
class FfmpegRunner final : public QObject
{
    Q_OBJECT

public:
    explicit FfmpegRunner(QString executable, QObject *parent = nullptr);
    ~FfmpegRunner() override;

    // The executable resolves and answers -version; the answer is kept until the resolved file changes.
    bool isAvailable() const;
    bool isRunning() const;
    QString commandLine(const QStringList &arguments) const;
    // Starts ffmpeg, and reports finished once it exits, from the event loop; false when one is running.
    bool run(const QStringList &arguments, const QString &outputPath);
    // Stops the running ffmpeg; finished then reports a failure.
    void cancel();

    // Arguments that encode input into output (in the container its extension names, at fps: WebM as VP9,
    // animated WebP, AVI as MJPEG, MP4 as H.264 in yuv420p with its index first). With loop, the output plays
    // input forward then backward, without repeating the last frame or ending on the first: 2N - 2 frames
    // from N. quality (0-100, higher is better) is the request's compressionQuality, mapped onto the
    // encoder's scale; without it, every encoder uses the equivalent of 90.
    static QStringList encodeArguments(const QString &input, const QString &output, int fps, bool loop,
                                       std::optional<int> quality = std::nullopt);

signals:
    void finished(const FfmpegResult &result);

private:
    QString resolvedExecutable() const;
    void finish(bool succeeded, int exitCode);

    QString m_executable;
    QProcess m_process;
    bool m_active = false;
    QStringList m_arguments;
    QString m_outputPath;
    QString m_standardError;
    mutable QString m_probedPath;
    mutable QDateTime m_probedModified;
    mutable bool m_probedAvailable = false;
};
