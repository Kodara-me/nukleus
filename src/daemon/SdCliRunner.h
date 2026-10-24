#pragma once

#include "GenerationRequest.h"
#include <nukleus/GenerationProgress.h>
#include "ServiceJobResult.h"
#include "SdCliProgressParser.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

class QProcess;

// What sd-cli's --help text says about the options image.animate validates against it.
struct SdCliHelp
{
    QHash<QString, QStringList> choices; // sd-cli flag -> accepted names ("one of [...]", examples, presets).
    std::optional<int> defaultFps;
    std::optional<int> defaultSteps;
};

class SdCliRunner final : public QObject
{
    Q_OBJECT

public:
    explicit SdCliRunner(const QString &executablePath, QObject *parent = nullptr);

    QString executablePath() const;
    // Parses the names listed for Nukleus::Protocol::animationHelpChoiceFlags() and the --fps default.
    static SdCliHelp parseHelp(const QString &help);
    QString commandLine(const QStringList& arguments) const;
    void runHelp();
    bool runGeneration(const GenerationRequest &request);
    // Runs sd-cli -M vid_gen with complete arguments; progress counts both sampling passes up to totalSteps,
    // or up to the total read from sd-cli's counters when it is unknown. Reports generationFinished. With
    // postProcessed, its success reports no saving or completion progress: the post-processing step does.
    bool runVideo(const QStringList &arguments, const QString &outputPath, std::optional<int> totalSteps, bool twoPasses,
                  bool postProcessed = false);
    bool runUpscale(const QStringList &arguments, const QString &outputPath);
    void cancel();
    std::optional<SdCliGenerationResult> lastGenerationResult() const;

signals:
    void helpFinished(bool succeeded);
    void generationOutput(const QString &stream, const QString &text);
    void generationProgressChanged(const Nukleus::GenerationProgress &progress);
    void generationFinished(const SdCliGenerationResult &result);
    void upscaleFinished(const SdCliUpscaleResult &result);

private:
    enum class RequestType
    {
        None,
        Help,
        Generation,
        Upscale
    };

    bool start(RequestType requestType, const QStringList &arguments, const QString &outputPath = {});
    void emitGenerationFinished(bool succeeded, int exitCode);
    void emitUpscaleFinished(bool succeeded, int exitCode);
    void appendGenerationOutput(const QByteArray &data, bool standardError);
    void flushGenerationOutput(bool standardError);
    void processGenerationLine(const QString &line);
    void logGenerationStart(const GenerationRequest &request, const QStringList &arguments) const;
    void logGenerationFinish(bool succeeded, int exitCode) const;

    QString m_executablePath;
    QProcess *m_process;
    RequestType m_requestType = RequestType::None;
    QStringList m_arguments;
    QString m_outputPath;
    bool m_memoryFailure = false;
    QString m_standardOutput;
    QString m_standardError;
    QString m_pendingStandardOutput;
    QString m_pendingStandardError;
    QElapsedTimer m_generationElapsed;
    SdCliProgressParser m_progressParser;
    std::optional<SdCliGenerationResult> m_lastGenerationResult;
};
