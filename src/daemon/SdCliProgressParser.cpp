#include "SdCliProgressParser.h"

#include <QObject>
#include <QRegularExpression>

#include <algorithm>

Nukleus::GenerationProgress SdCliProgressParser::begin(int samplingSteps)
{
    m_samplingSteps = samplingSteps;
    m_percentage = 0;
    m_stage = Nukleus::GenerationProgressStage::Preparing;
    m_samplingStarted = false;
    m_currentStep.reset();
    m_hasProgress = true;
    m_video = false;
    m_postProcessed = false;
    return {Nukleus::GenerationProgressStage::Preparing,
            std::nullopt,
            std::nullopt,
            0,
            QObject::tr("Preparing generation…")};
}

Nukleus::GenerationProgress SdCliProgressParser::beginVideo(std::optional<int> totalSteps, bool twoPasses,
                                                            bool postProcessed)
{
    auto progress = begin(totalSteps.value_or(0));
    m_video = true;
    m_postProcessed = postProcessed;
    m_expectedTotalSteps = totalSteps && *totalSteps > 0 ? totalSteps : std::nullopt;
    m_completedPassSteps = 0;
    m_passSteps = 0;
    m_passIndex = 0;
    m_twoPasses = twoPasses;
    m_samplingDone = false;
    progress.statusText = QObject::tr("Preparing animation…");
    return progress;
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::starting()
{
    return update(Nukleus::GenerationProgressStage::Starting, 2,
                  m_video ? QObject::tr("Starting animation…") : QObject::tr("Starting image generation…"));
}

int SdCliProgressParser::videoTotalSteps(int passSteps) const
{
    // sd-cli may adjust the step count (custom sigmas), so the counters raise the expected total when needed.
    // Without an expected total, the second pass is estimated as long as the first until it starts.
    const int remainingPass = !m_expectedTotalSteps && m_twoPasses && m_passIndex == 0 ? passSteps : 0;
    return std::max(m_expectedTotalSteps.value_or(0), m_completedPassSteps + passSteps + remainingPass);
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::processVideoLine(const QString &line)
{
    using Stage = Nukleus::GenerationProgressStage;
    static const QRegularExpression loading(QStringLiteral("loading (high noise )?(diffusion )?model from"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression generating(QStringLiteral("generate_video \\d+x\\d+x\\d+"));
    if (loading.match(line).hasMatch())
        return update(Stage::LoadingModel, 5, QObject::tr("Loading animation model…"));
    // Printed once the conditions are ready, just before the first sampling step.
    if (!m_samplingStarted && generating.match(line).hasMatch()) {
        m_samplingStarted = true;
        if (!m_expectedTotalSteps) return update(Stage::Sampling, 15, QObject::tr("Sampling video…"));
        return update(Stage::Sampling, 15, QObject::tr("Sampling video %1 / %2").arg(1).arg(*m_expectedTotalSteps),
                      1, *m_expectedTotalSteps);
    }
    if (m_samplingStarted && !m_samplingDone && line.contains(QStringLiteral("sampling(high noise) completed"))) {
        m_completedPassSteps += m_passSteps;
        m_passSteps = 0;
        ++m_passIndex;
        return std::nullopt;
    }
    if (m_samplingStarted && !m_samplingDone && line.contains(QStringLiteral("sampling completed"))) {
        m_samplingDone = true;
        m_completedPassSteps += m_passSteps;
        m_passSteps = 0;
        ++m_passIndex;
        const int total = videoTotalSteps(0);
        if (total <= 0) return update(Stage::Sampling, 90, QObject::tr("Sampling video…"));
        return update(Stage::Sampling, 90, QObject::tr("Sampling video %1 / %2").arg(total).arg(total), total, total);
    }
    if (line.contains(QStringLiteral("generating latent video completed")))
        return update(Stage::Decoding, 94, QObject::tr("Decoding video…"));
    // A post-processed video is saved once the post-processing step has written it.
    if (line.contains(QStringLiteral("save result video")))
        return m_postProcessed ? std::nullopt : update(Stage::Saving, 99, QObject::tr("Saving video…"));
    static const QRegularExpression samplingCounter(QStringLiteral("\\b(\\d+)\\s*/\\s*(\\d+)\\b"));
    return videoSamplingProgress(samplingCounter.match(line));
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::videoSamplingProgress(const QRegularExpressionMatch &match)
{
    // Counters before sampling (loading) or after it (VAE tiling) are not sampling steps.
    if (!m_samplingStarted || m_samplingDone || !match.hasMatch()) return std::nullopt;
    const int completedSteps = match.captured(1).toInt();
    const int passSteps = match.captured(2).toInt();
    if (passSteps <= 0 || completedSteps < 0 || completedSteps > passSteps) return std::nullopt;
    m_passSteps = passSteps;
    const int total = videoTotalSteps(passSteps);
    const int completed = m_completedPassSteps + completedSteps;
    // As for images, the step being sampled is shown and the percentage follows completed steps.
    const int runningStep = std::min(completed + 1, total);
    const int percentage = 15 + (completed * 75 + total / 2) / total;
    return update(Nukleus::GenerationProgressStage::Sampling, percentage,
                  QObject::tr("Sampling video %1 / %2").arg(runningStep).arg(total), runningStep, total);
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::processLine(const QString &line)
{
    if (m_video) return processVideoLine(line);
    if (line.contains(QStringLiteral("loading model from"), Qt::CaseInsensitive)) {
        return update(Nukleus::GenerationProgressStage::LoadingModel, 5, QObject::tr("Loading image generation model…"));
    }
    // Printed just before the first step, once sampling has started: step 1 is running.
    if (m_samplingStarted && m_samplingSteps > 0
        && line.contains(QStringLiteral("sampling using"), Qt::CaseInsensitive)) {
        return update(Nukleus::GenerationProgressStage::Sampling, 15,
                      QObject::tr("Sampling %1 / %2").arg(1).arg(m_samplingSteps), 1, m_samplingSteps);
    }
    if (line.contains(QStringLiteral("generate_image"), Qt::CaseInsensitive)
        || line.contains(QStringLiteral("sampling using"), Qt::CaseInsensitive)) {
        return update(Nukleus::GenerationProgressStage::Starting, 10, QObject::tr("Starting image generation…"));
    }
    if (line.contains(QStringLiteral("generating image:"), Qt::CaseInsensitive)) {
        m_samplingStarted = true;
        return update(Nukleus::GenerationProgressStage::Sampling, 15, QObject::tr("Sampling image…"));
    }
    if (line.contains(QStringLiteral("sampling completed"), Qt::CaseInsensitive)) {
        return update(Nukleus::GenerationProgressStage::Sampling,
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
        return update(Nukleus::GenerationProgressStage::Decoding, 94, QObject::tr("Decoding image…"));
    }
    if (line.contains(QStringLiteral("save result image"), Qt::CaseInsensitive)) {
        return update(Nukleus::GenerationProgressStage::Saving, 99, QObject::tr("Saving image…"));
    }

    static const QRegularExpression samplingCounter(QStringLiteral("\\b(\\d+)\\s*/\\s*(\\d+)\\b"));
    return samplingProgress(samplingCounter.match(line));
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::processPartialLine(const QString &text)
{
    // sd-cli redraws its bar as "\r  |===>   | 19/20 - 1.23s/it" and only ends the line after the
    // last step, so a counter is complete before its terminator arrives. Require the " -" that
    // follows it, so a number cut in half by a read is never taken.
    static const QRegularExpression completeCounter(QStringLiteral("\\b(\\d+)\\s*/\\s*(\\d+)\\s+-"));
    return m_video ? videoSamplingProgress(completeCounter.match(text)) : samplingProgress(completeCounter.match(text));
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::samplingProgress(const QRegularExpressionMatch &match)
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
    return update(Nukleus::GenerationProgressStage::Sampling,
                  percentage,
                  QObject::tr("Sampling %1 / %2").arg(runningStep).arg(totalSteps),
                  runningStep,
                  totalSteps);
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::complete()
{
    if (m_postProcessed) return std::nullopt;
    return update(Nukleus::GenerationProgressStage::Complete, 100,
                  m_video ? QObject::tr("Animation complete") : QObject::tr("Generation complete"));
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::failed()
{
    return update(Nukleus::GenerationProgressStage::Failed, m_percentage,
                  m_video ? QObject::tr("Animation failed") : QObject::tr("Generation failed"));
}

std::optional<Nukleus::GenerationProgress> SdCliProgressParser::update(Nukleus::GenerationProgressStage stage,
                                                                int percentage,
                                                                const QString &statusText,
                                                                std::optional<int> currentStep,
                                                                std::optional<int> totalSteps)
{
    if (stage != Nukleus::GenerationProgressStage::Failed && m_hasProgress && percentage < m_percentage) {
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
    return Nukleus::GenerationProgress{stage, currentStep, totalSteps, percentage, statusText};
}
