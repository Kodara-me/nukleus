#pragma once

#include "GenerationProgress.h"

#include <optional>

class QString;

class SdCliProgressParser final
{
public:
    GenerationProgress begin(int samplingSteps);
    std::optional<GenerationProgress> starting();
    std::optional<GenerationProgress> processLine(const QString &line);
    std::optional<GenerationProgress> complete();
    std::optional<GenerationProgress> failed();

private:
    std::optional<GenerationProgress> update(GenerationProgressStage stage,
                                             int percentage,
                                             const QString &statusText,
                                             std::optional<int> currentStep = std::nullopt,
                                             std::optional<int> totalSteps = std::nullopt);

    int m_samplingSteps = 0;
    int m_percentage = 0;
    GenerationProgressStage m_stage = GenerationProgressStage::Preparing;
    bool m_samplingStarted = false;
    bool m_hasProgress = false;
};
