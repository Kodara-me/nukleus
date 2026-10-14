#include "SdCliProgressParser.h"

#include <QObject>
#include <QRegularExpression>

#include <algorithm>

KiraService::GenerationProgress SdCliProgressParser::begin(int samplingSteps)
{
    m_samplingSteps = samplingSteps;
    m_percentage = 0;
    m_stage = KiraService::GenerationProgressStage::Preparing;
    m_samplingStarted = false;
    m_currentStep.reset();
    m_hasProgress = true;
    return {KiraService::GenerationProgressStage::Preparing,
            std::nullopt,
            std::nullopt,
            0,
            QObject::tr("Preparing generation…")};
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::starting()
{
    return update(KiraService::GenerationProgressStage::Starting, 2, QObject::tr("Starting image generation…"));
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::processLine(const QString &line)
{
    if (line.contains(QStringLiteral("loading model from"), Qt::CaseInsensitive)) {
        return update(KiraService::GenerationProgressStage::LoadingModel, 5, QObject::tr("Loading image generation model…"));
    }
    // Printed just before the first step, once sampling has started: step 1 is running.
    if (m_samplingStarted && m_samplingSteps > 0
        && line.contains(QStringLiteral("sampling using"), Qt::CaseInsensitive)) {
        return update(KiraService::GenerationProgressStage::Sampling, 15,
                      QObject::tr("Sampling %1 / %2").arg(1).arg(m_samplingSteps), 1, m_samplingSteps);
    }
    if (line.contains(QStringLiteral("generate_image"), Qt::CaseInsensitive)
        || line.contains(QStringLiteral("sampling using"), Qt::CaseInsensitive)) {
        return update(KiraService::GenerationProgressStage::Starting, 10, QObject::tr("Starting image generation…"));
    }
    if (line.contains(QStringLiteral("generating image:"), Qt::CaseInsensitive)) {
        m_samplingStarted = true;
        return update(KiraService::GenerationProgressStage::Sampling, 15, QObject::tr("Sampling image…"));
    }
    if (line.contains(QStringLiteral("sampling completed"), Qt::CaseInsensitive)) {
        return update(KiraService::GenerationProgressStage::Sampling,
                      90,
                      QObject::tr("Sampling %1 / %2").arg(m_samplingSteps).arg(m_samplingSteps),
                      m_samplingSteps,
                      m_samplingSteps);
    }
    // Match "decoding 1 latents" only: sd-cli also prints "using VAE for encoding / decoding"
    // while loading, which must not jump to the decoding stage before sampling.
    static const QRegularExpression decodingLatents(QStringLiteral("\\bdecoding \\d+ latents?\\b"),
                                                    QRegularExpression::CaseInsensitiveOption);
    if (decodingLatents.match(line).hasMatch()
        || line.contains(QStringLiteral("decode_first_stage"), Qt::CaseInsensitive)) {
        return update(KiraService::GenerationProgressStage::Decoding, 94, QObject::tr("Decoding image…"));
    }
    if (line.contains(QStringLiteral("save result image"), Qt::CaseInsensitive)) {
        return update(KiraService::GenerationProgressStage::Saving, 99, QObject::tr("Saving image…"));
    }

    static const QRegularExpression samplingCounter(QStringLiteral("\\b(\\d+)\\s*/\\s*(\\d+)\\b"));
    return samplingProgress(samplingCounter.match(line));
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::processPartialLine(const QString &text)
{
    // sd-cli redraws its bar as "\r  |===>   | 19/20 - 1.23s/it" and only ends the line after the
    // last step, so a counter is complete before its terminator arrives. Require the " -" that
    // follows it, so a number cut in half by a read is never taken.
    static const QRegularExpression completeCounter(QStringLiteral("\\b(\\d+)\\s*/\\s*(\\d+)\\s+-"));
    return samplingProgress(completeCounter.match(text));
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::samplingProgress(const QRegularExpressionMatch &match)
{
    if (!m_samplingStarted || m_samplingSteps <= 0 || !match.hasMatch()) {
        return std::nullopt;
    }

    const int completedSteps = match.captured(1).toInt();
    const int totalSteps = match.captured(2).toInt();
    if (completedSteps < 0 || completedSteps > totalSteps || totalSteps != m_samplingSteps) {
        return std::nullopt;
    }

    // sd-cli counts completed steps; clients show the step being sampled, so the last step
    // reads N / N while it runs. The percentage follows completed steps.
    const int runningStep = std::min(completedSteps + 1, totalSteps);
    const int percentage = 15 + (completedSteps * 75 + totalSteps / 2) / totalSteps;
    return update(KiraService::GenerationProgressStage::Sampling,
                  percentage,
                  QObject::tr("Sampling %1 / %2").arg(runningStep).arg(totalSteps),
                  runningStep,
                  totalSteps);
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::complete()
{
    return update(KiraService::GenerationProgressStage::Complete, 100, QObject::tr("Generation complete"));
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::failed()
{
    return update(KiraService::GenerationProgressStage::Failed, m_percentage, QObject::tr("Generation failed"));
}

std::optional<KiraService::GenerationProgress> SdCliProgressParser::update(KiraService::GenerationProgressStage stage,
                                                                int percentage,
                                                                const QString &statusText,
                                                                std::optional<int> currentStep,
                                                                std::optional<int> totalSteps)
{
    if (stage != KiraService::GenerationProgressStage::Failed && m_hasProgress && percentage < m_percentage) {
        return std::nullopt;
    }
    percentage = std::max(percentage, m_percentage);
    // A counter read early from a partial line arrives again when its line ends.
    if (m_hasProgress && stage == m_stage && percentage == m_percentage
        && (currentStep == std::nullopt || currentStep == m_currentStep)) {
        return std::nullopt;
    }
    // The running step only moves forward.
    if (currentStep && m_currentStep && stage == m_stage && *currentStep < *m_currentStep) {
        return std::nullopt;
    }

    m_currentStep = currentStep ? currentStep : m_currentStep;
    m_stage = stage;
    m_percentage = percentage;
    m_hasProgress = true;
    return KiraService::GenerationProgress{stage, currentStep, totalSteps, percentage, statusText};
}
