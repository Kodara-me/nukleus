#pragma once

#include <nukleus/GenerationProgress.h>

#include <optional>

class QRegularExpressionMatch;
class QString;

class SdCliProgressParser final
{
public:
    Nukleus::GenerationProgress begin(int samplingSteps);
    // Video mode (sd-cli -M vid_gen): sampling runs a high-noise pass then a low-noise pass, each with its own
    // "n/N" counter; the running step counts both. Without an expected total, the counters give it, and a
    // second pass (twoPasses) is assumed as long as the first until its own counter arrives.
    // With postProcessed, the video is not final when sd-cli saves it: its saving and completion are left
    // to the post-processing step.
    Nukleus::GenerationProgress beginVideo(std::optional<int> totalSteps, bool twoPasses = true,
                                           bool postProcessed = false);
    std::optional<Nukleus::GenerationProgress> starting();
    std::optional<Nukleus::GenerationProgress> processLine(const QString &line);
    // Reads a complete step counter from output whose line has not ended yet.
    std::optional<Nukleus::GenerationProgress> processPartialLine(const QString &text);
    std::optional<Nukleus::GenerationProgress> complete();
    std::optional<Nukleus::GenerationProgress> failed();

private:
    std::optional<Nukleus::GenerationProgress> samplingProgress(const QRegularExpressionMatch &match);
    std::optional<Nukleus::GenerationProgress> processVideoLine(const QString &line);
    std::optional<Nukleus::GenerationProgress> videoSamplingProgress(const QRegularExpressionMatch &match);
    int videoTotalSteps(int passSteps) const;
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
    bool m_video = false;
    std::optional<int> m_expectedTotalSteps;
    int m_completedPassSteps = 0; // Steps of the sampling passes already finished.
    int m_passSteps = 0;          // The running pass's step count, from its counter.
    int m_passIndex = 0;
    bool m_twoPasses = true;
    bool m_samplingDone = false;  // Later counters belong to VAE tiling, not sampling.
    bool m_postProcessed = false;
};
