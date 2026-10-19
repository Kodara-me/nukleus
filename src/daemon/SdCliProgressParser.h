#pragma once

#include <nukleus/GenerationProgress.h>

#include <optional>

class QRegularExpressionMatch;
class QString;

class SdCliProgressParser final
{
public:
    Nukleus::GenerationProgress begin(int samplingSteps);
    std::optional<Nukleus::GenerationProgress> starting();
    std::optional<Nukleus::GenerationProgress> processLine(const QString &line);
    // Reads a complete step counter from output whose line has not ended yet.
    std::optional<Nukleus::GenerationProgress> processPartialLine(const QString &text);
    std::optional<Nukleus::GenerationProgress> complete();
    std::optional<Nukleus::GenerationProgress> failed();

private:
    std::optional<Nukleus::GenerationProgress> samplingProgress(const QRegularExpressionMatch &match);
    std::optional<Nukleus::GenerationProgress> update(Nukleus::GenerationProgressStage stage,
                                             int percentage,
                                             const QString &statusText,
                                             std::optional<int> currentStep = std::nullopt,
                                             std::optional<int> totalSteps = std::nullopt);

    int m_samplingSteps = 0;
    int m_percentage = 0;
    Nukleus::GenerationProgressStage m_stage = Nukleus::GenerationProgressStage::Preparing;
    bool m_samplingStarted = false;
    std::optional<int> m_currentStep;
    bool m_hasProgress = false;
};
