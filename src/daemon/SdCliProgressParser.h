#pragma once

#include <kira/service/GenerationProgress.h>

#include <optional>

class QRegularExpressionMatch;
class QString;

class SdCliProgressParser final
{
public:
    KiraService::GenerationProgress begin(int samplingSteps);
    std::optional<KiraService::GenerationProgress> starting();
    std::optional<KiraService::GenerationProgress> processLine(const QString &line);
    // Reads a complete step counter from output whose line has not ended yet.
    std::optional<KiraService::GenerationProgress> processPartialLine(const QString &text);
    std::optional<KiraService::GenerationProgress> complete();
    std::optional<KiraService::GenerationProgress> failed();

private:
    std::optional<KiraService::GenerationProgress> samplingProgress(const QRegularExpressionMatch &match);
    std::optional<KiraService::GenerationProgress> update(KiraService::GenerationProgressStage stage,
                                             int percentage,
                                             const QString &statusText,
                                             std::optional<int> currentStep = std::nullopt,
                                             std::optional<int> totalSteps = std::nullopt);

    int m_samplingSteps = 0;
    int m_percentage = 0;
    KiraService::GenerationProgressStage m_stage = KiraService::GenerationProgressStage::Preparing;
    bool m_samplingStarted = false;
    std::optional<int> m_currentStep;
    bool m_hasProgress = false;
};
