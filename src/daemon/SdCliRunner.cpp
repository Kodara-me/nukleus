#include "SdCliRunner.h"

#include <QDebug>
#include <QProcess>
#include <QTimer>

#include <algorithm>

namespace
{
QString quotedCommandArgument(const QString &argument)
{
    QString quotedArgument = argument;
    const QString escapedQuote = QString(QLatin1Char('\'')) + QLatin1Char('\\') + QLatin1Char('\'')
                                 + QLatin1Char('\'');
    quotedArgument.replace(QLatin1Char('\''), escapedQuote);
    return QLatin1Char('\'') + quotedArgument + QLatin1Char('\'');
}
}

SdCliRunner::SdCliRunner(const QString &executablePath, QObject *parent)
    : QObject(parent)
    , m_executablePath(executablePath)
    , m_process(new QProcess(this))
{
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    qRegisterMetaType<SdCliGenerationResult>();
    qRegisterMetaType<SdCliUpscaleResult>();
    qRegisterMetaType<KiraService::GenerationProgress>();
    connect(m_process, &QProcess::started, this, [this] {
        if (m_requestType != RequestType::Generation) {
            return;
        }
        if (const auto progress = m_progressParser.starting()) {
            emit generationProgressChanged(*progress);
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        if (m_requestType == RequestType::Generation) {
            appendGenerationOutput(m_process->readAllStandardOutput(), false);
        }
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        if (m_requestType == RequestType::Generation) {
            appendGenerationOutput(m_process->readAllStandardError(), true);
        }
    });
    connect(m_process,
            &QProcess::finished,
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_requestType == RequestType::None) {
                    return;
                }
                const RequestType completedRequest = m_requestType;
                m_requestType = RequestType::None;
                const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
                if (completedRequest == RequestType::Help) {
                    emit helpFinished(succeeded);
                } else if (completedRequest == RequestType::Generation) {
                    appendGenerationOutput(m_process->readAllStandardOutput(), false);
                    appendGenerationOutput(m_process->readAllStandardError(), true);
                    emitGenerationFinished(succeeded, exitCode);
                } else if (completedRequest == RequestType::Upscale) {
                    emitUpscaleFinished(succeeded, exitCode);
                }
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_requestType == RequestType::None || error != QProcess::FailedToStart) {
            return;
        }

        const RequestType failedRequest = m_requestType;
        m_requestType = RequestType::None;
        if (failedRequest == RequestType::Help) {
            emit helpFinished(false);
        } else if (failedRequest == RequestType::Generation) {
            qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Process error: %1").arg(m_process->errorString());
            emitGenerationFinished(false, -1);
        } else if (failedRequest == RequestType::Upscale) {
            emitUpscaleFinished(false, -1);
        }
    });
}

QString SdCliRunner::executablePath() const
{
    return m_executablePath;
}

void SdCliRunner::runHelp()
{
    start(RequestType::Help, {QStringLiteral("--help")});
}

bool SdCliRunner::runGeneration(const GenerationRequest &request)
{
    QStringList arguments = {
        QStringLiteral("--model"), request.modelPath,
        QStringLiteral("--prompt"), request.prompt,
        QStringLiteral("--negative-prompt"), request.negativePrompt,
        QStringLiteral("--width"), QString::number(request.width),
        QStringLiteral("--height"), QString::number(request.height),
        QStringLiteral("--steps"), QString::number(request.steps),
        QStringLiteral("--cfg-scale"), QString::number(request.cfgScale, 'f', 1),
        QStringLiteral("--sampling-method"), request.sampler,
        QStringLiteral("--scheduler"), request.scheduler,
        QStringLiteral("--seed"), QString::number(request.seed),
        QStringLiteral("--output"), request.outputPath};
    if (!request.baseImagePath.isEmpty()) {
        arguments.append({QStringLiteral("--init-img"), request.baseImagePath,
                          QStringLiteral("--strength"), QString::number(request.baseImageStrength, 'f', 2)});
    }
    if (m_requestType != RequestType::None || m_executablePath.isEmpty()) {
        return false;
    }

    m_standardOutput.clear();
    m_standardError.clear();
    m_pendingStandardOutput.clear();
    m_pendingStandardError.clear();
    m_generationElapsed.start();
    emit generationProgressChanged(m_progressParser.begin(request.steps));
    logGenerationStart(request, arguments);
    return start(RequestType::Generation, arguments, request.outputPath);
}

bool SdCliRunner::runUpscale(const QStringList &arguments, const QString &outputPath)
{
    return start(RequestType::Upscale, arguments, outputPath);
}

void SdCliRunner::cancel()
{
    if (m_requestType == RequestType::None || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_process->terminate();
    QTimer::singleShot(3000, m_process, [process = m_process] {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
}

std::optional<SdCliGenerationResult> SdCliRunner::lastGenerationResult() const
{
    return m_lastGenerationResult;
}

bool SdCliRunner::start(RequestType requestType, const QStringList &arguments, const QString &outputPath)
{
    if (m_requestType != RequestType::None || m_executablePath.isEmpty()) {
        return false;
    }

    m_requestType = requestType;
    m_arguments = arguments;
    m_outputPath = outputPath;
    m_process->start(m_executablePath, arguments);
    return true;
}

void SdCliRunner::emitGenerationFinished(bool succeeded, int exitCode)
{
    flushGenerationOutput(false);
    flushGenerationOutput(true);
    const std::optional<KiraService::GenerationProgress> progress = succeeded ? m_progressParser.complete() : m_progressParser.failed();
    if (progress.has_value()) {
        emit generationProgressChanged(*progress);
    }
    m_lastGenerationResult = {succeeded,
                              exitCode,
                              m_arguments,
                              m_standardOutput,
                              m_standardError,
                              m_outputPath};
    logGenerationFinish(succeeded, exitCode);
    m_arguments.clear();
    m_outputPath.clear();
    emit generationFinished(*m_lastGenerationResult);
}

void SdCliRunner::emitUpscaleFinished(bool succeeded, int exitCode)
{
    const SdCliUpscaleResult result = {succeeded, exitCode, m_outputPath};
    m_arguments.clear();
    m_outputPath.clear();
    emit upscaleFinished(result);
}

void SdCliRunner::appendGenerationOutput(const QByteArray &data, bool standardError)
{
    if (data.isEmpty()) {
        return;
    }

    const QString text = QString::fromLocal8Bit(data);
    QString &collected = standardError ? m_standardError : m_standardOutput;
    QString &pending = standardError ? m_pendingStandardError : m_pendingStandardOutput;
    collected.append(text);
    pending.append(text);

    qsizetype newlineIndex = pending.indexOf(QLatin1Char('\n'));
    qsizetype carriageReturnIndex = pending.indexOf(QLatin1Char('\r'));
    while (newlineIndex >= 0 || carriageReturnIndex >= 0) {
        const qsizetype lineEnd = newlineIndex < 0 ? carriageReturnIndex
                                                    : (carriageReturnIndex < 0 ? newlineIndex
                                                                              : std::min(newlineIndex, carriageReturnIndex));
        const QString line = pending.left(lineEnd);
        pending.remove(0, lineEnd + 1);
        if (!line.isEmpty()) {
            const QString stream = standardError ? QStringLiteral("stderr") : QStringLiteral("stdout");
            qInfo().noquote() << QStringLiteral("[sd-cli][%1] %2").arg(stream, line);
            emit generationOutput(stream, line);
            processGenerationLine(line);
        }
        newlineIndex = pending.indexOf(QLatin1Char('\n'));
        carriageReturnIndex = pending.indexOf(QLatin1Char('\r'));
    }
}

void SdCliRunner::flushGenerationOutput(bool standardError)
{
    QString &pending = standardError ? m_pendingStandardError : m_pendingStandardOutput;
    if (pending.isEmpty()) {
        return;
    }

    const QString stream = standardError ? QStringLiteral("stderr") : QStringLiteral("stdout");
    qInfo().noquote() << QStringLiteral("[sd-cli][%1] %2").arg(stream, pending);
    emit generationOutput(stream, pending);
    processGenerationLine(pending);
    pending.clear();
}

void SdCliRunner::processGenerationLine(const QString &line)
{
    if (const std::optional<KiraService::GenerationProgress> progress = m_progressParser.processLine(line)) {
        emit generationProgressChanged(*progress);
    }
}

void SdCliRunner::logGenerationStart(const GenerationRequest &request, const QStringList &arguments) const
{
    QStringList displayedArguments;
    displayedArguments.reserve(arguments.size());
    for (const QString &argument : arguments) {
        displayedArguments.append(quotedCommandArgument(argument));
    }

    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Starting text-to-image generation");
    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Executable: %1").arg(m_executablePath);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Resolution: %1x%2").arg(request.width).arg(request.height);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Steps: %1").arg(request.steps);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] CFG: %1").arg(request.cfgScale, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Sampler: %1").arg(request.sampler);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Scheduler: %1").arg(request.scheduler);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Seed: %1").arg(request.seed);
    if (!request.baseImagePath.isEmpty()) {
        qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Base image: %1").arg(request.baseImagePath);
        qInfo().noquote() << QStringLiteral("[Kira Canvas][generation] Base image strength: %1")
                                 .arg(request.baseImageStrength, 0, 'f', 2);
    }
    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Command:\n%1 %2")
                             .arg(quotedCommandArgument(m_executablePath), displayedArguments.join(QLatin1Char(' ')));
}

void SdCliRunner::logGenerationFinish(bool succeeded, int exitCode) const
{
    const double elapsedSeconds = m_generationElapsed.isValid() ? m_generationElapsed.elapsed() / 1000.0 : 0.0;
    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Finished");
    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Exit code: %1").arg(exitCode);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Output: %1").arg(m_outputPath);
    qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Elapsed: %1 s").arg(elapsedSeconds, 0, 'f', 1);
    if (!succeeded && !m_standardError.isEmpty()) {
        qInfo().noquote() << QStringLiteral("[Kira Canvas][sd-cli] Generation failed; stderr was captured above.");
    }
}
