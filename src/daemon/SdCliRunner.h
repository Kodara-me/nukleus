#pragma once

#include "GenerationRequest.h"
#include <kira/service/GenerationProgress.h>
#include "ServiceJobResult.h"
#include "SdCliProgressParser.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

class QProcess;

class SdCliRunner final : public QObject
{
    Q_OBJECT

public:
    explicit SdCliRunner(const QString &executablePath, QObject *parent = nullptr);

    QString executablePath() const;
    QString commandLine(const QStringList& arguments) const;
    void runHelp();
    bool runGeneration(const GenerationRequest &request);
    bool runUpscale(const QStringList &arguments, const QString &outputPath);
    void cancel();
    std::optional<SdCliGenerationResult> lastGenerationResult() const;

signals:
    void helpFinished(bool succeeded);
    void generationOutput(const QString &stream, const QString &text);
    void generationProgressChanged(const KiraService::GenerationProgress &progress);
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
