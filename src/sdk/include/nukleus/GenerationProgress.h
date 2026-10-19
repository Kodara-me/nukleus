#pragma once

#include <QString>
#include <QMetaType>

#include <optional>

namespace Nukleus
{
enum class GenerationProgressStage
{
    Preparing,
    EnhancingPrompt,
    Starting,
    LoadingModel,
    Sampling,
    Decoding,
    Saving,
    Complete,
    Failed
};

struct GenerationProgress
{
    GenerationProgressStage stage = GenerationProgressStage::Preparing;
    std::optional<int> currentStep;
    std::optional<int> totalSteps;
    int percentage = 0;
    QString statusText;
};
}

Q_DECLARE_METATYPE(Nukleus::GenerationProgress)
