#pragma once

#include <QString>
#include <QMetaType>

#include <optional>

namespace KiraService
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
    // Prompt-flow position (1-based) and bilingual step name during EnhancingPrompt.
    std::optional<int> flowStep = {};
    std::optional<int> flowSteps = {};
    QString flowStepName = {};
    QString flowStepNameFr = {};
};
}

Q_DECLARE_METATYPE(KiraService::GenerationProgress)
