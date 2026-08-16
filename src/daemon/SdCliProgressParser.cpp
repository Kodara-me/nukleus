#include "SdCliProgressParser.h"

#include <QObject>
#include <QRegularExpression>

#include <algorithm>

GenerationProgress SdCliProgressParser::begin(int samplingSteps)
{
    m_samplingSteps = samplingSteps;
    m_percentage = 0;
    m_stage = GenerationProgressStage::Preparing;
    m_samplingStarted = false;
    m_hasProgress = true;
    return {GenerationProgressStage::Preparing,
            std::nullopt,
            std::nullopt,
            0,
            QObject::tr("Preparing generation…")};
}

std::optional<GenerationProgress> SdCliProgressParser::starting()
{
    return update(GenerationProgressStage::Starting, 2, QObject::tr("Starting image generation…"));
}

std::optional<GenerationProgress> SdCliProgressParser::processLine(const QString &line)
{
    if (line.contains(QStringLiteral("loading model from"), Qt::CaseInsensitive)) {
        return update(GenerationProgressStage::LoadingModel, 5, QObject::tr("Loading image generation model…"));
    }
    if (line.contains(QStringLiteral("generate_image"), Qt::CaseInsensitive)
        || line.contains(QStringLiteral("sampling using"), Qt::CaseInsensitive)) {
        return update(GenerationProgressStage::Starting, 10, QObject::tr("Starting image generation…"));
    }
    if (line.contains(QStringLiteral("generating image:"), Qt::CaseInsensitive)) {
        m_samplingStarted = true;
        return update(GenerationProgressStage::Sampling, 15, QObject::tr("Sampling image…"));
    }
    if (line.contains(QStringLiteral("sampling completed"), Qt::CaseInsensitive)) {
        return update(GenerationProgressStage::Sampling,
                      90,
                      QObject::tr("Sampling %1 / %2").arg(m_samplingSteps).arg(m_samplingSteps),
                      m_samplingSteps,
                      m_samplingSteps);
    }
    if (line.contains(QStringLiteral("decoding "), Qt::CaseInsensitive)
        || line.contains(QStringLiteral("decode_first_stage"), Qt::CaseInsensitive)) {
        return update(GenerationProgressStage::Decoding, 94, QObject::tr("Decoding image…"));
    }
    if (line.contains(QStringLiteral("save result image"), Qt::CaseInsensitive)) {
        return update(GenerationProgressStage::Saving, 99, QObject::tr("Saving image…"));
    }

    if (!m_samplingStarted || m_samplingSteps <= 0) {
        return std::nullopt;
    }

    static const QRegularExpression samplingCounter(QStringLiteral("\\b(\\d+)\\s*/\\s*(\\d+)\\b"));
    const QRegularExpressionMatch match = samplingCounter.match(line);
    if (!match.hasMatch()) {
        return std::nullopt;
    }

    const int currentStep = match.captured(1).toInt();
    const int totalSteps = match.captured(2).toInt();
    if (currentStep < 0 || currentStep > totalSteps || totalSteps != m_samplingSteps) {
        return std::nullopt;
    }

    const int percentage = 15 + (currentStep * 75 + totalSteps / 2) / totalSteps;
    return update(GenerationProgressStage::Sampling,
                  percentage,
                  QObject::tr("Sampling %1 / %2").arg(currentStep).arg(totalSteps),
                  currentStep,
                  totalSteps);
}

std::optional<GenerationProgress> SdCliProgressParser::complete()
{
    return update(GenerationProgressStage::Complete, 100, QObject::tr("Generation complete"));
}

std::optional<GenerationProgress> SdCliProgressParser::failed()
{
    return update(GenerationProgressStage::Failed, m_percentage, QObject::tr("Generation failed"));
}

std::optional<GenerationProgress> SdCliProgressParser::update(GenerationProgressStage stage,
                                                                int percentage,
                                                                const QString &statusText,
                                                                std::optional<int> currentStep,
                                                                std::optional<int> totalSteps)
{
    if (stage != GenerationProgressStage::Failed && m_hasProgress && percentage < m_percentage) {
        return std::nullopt;
    }
    percentage = std::max(percentage, m_percentage);
    if (m_hasProgress && stage == m_stage && percentage == m_percentage && currentStep == std::nullopt) {
        return std::nullopt;
    }

    m_stage = stage;
    m_percentage = percentage;
    m_hasProgress = true;
    return GenerationProgress{stage, currentStep, totalSteps, percentage, statusText};
}
