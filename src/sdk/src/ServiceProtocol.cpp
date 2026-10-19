#include "nukleus/ServiceProtocol.h"

#include <QHash>

namespace Nukleus::Protocol
{
QString stageName(Nukleus::GenerationProgressStage stage)
{
    switch (stage) {
    case Nukleus::GenerationProgressStage::Preparing: return QStringLiteral("preparing");
    case Nukleus::GenerationProgressStage::EnhancingPrompt: return QStringLiteral("enhancing-prompt");
    case Nukleus::GenerationProgressStage::Starting: return QStringLiteral("starting");
    case Nukleus::GenerationProgressStage::LoadingModel: return QStringLiteral("loading-model");
    case Nukleus::GenerationProgressStage::Sampling: return QStringLiteral("sampling");
    case Nukleus::GenerationProgressStage::Decoding: return QStringLiteral("decoding");
    case Nukleus::GenerationProgressStage::Saving: return QStringLiteral("saving");
    case Nukleus::GenerationProgressStage::Complete: return QStringLiteral("complete");
    case Nukleus::GenerationProgressStage::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

Nukleus::GenerationProgressStage stageFromName(const QString &name)
{
    static const QHash<QString, Nukleus::GenerationProgressStage> stages = {
        {QStringLiteral("preparing"), Nukleus::GenerationProgressStage::Preparing},
        {QStringLiteral("enhancing-prompt"), Nukleus::GenerationProgressStage::EnhancingPrompt},
        {QStringLiteral("starting"), Nukleus::GenerationProgressStage::Starting},
        {QStringLiteral("loading-model"), Nukleus::GenerationProgressStage::LoadingModel},
        {QStringLiteral("sampling"), Nukleus::GenerationProgressStage::Sampling},
        {QStringLiteral("decoding"), Nukleus::GenerationProgressStage::Decoding},
        {QStringLiteral("saving"), Nukleus::GenerationProgressStage::Saving},
        {QStringLiteral("complete"), Nukleus::GenerationProgressStage::Complete},
        {QStringLiteral("failed"), Nukleus::GenerationProgressStage::Failed}};
    return stages.value(name, Nukleus::GenerationProgressStage::Failed);
}

QJsonObject progressToJson(const Nukleus::GenerationProgress &progress)
{
    QJsonObject result{{QStringLiteral("stage"), stageName(progress.stage)},
                       {QStringLiteral("percentage"), progress.percentage},
                       {QStringLiteral("statusText"), progress.statusText}};
    if (progress.currentStep) result.insert(QStringLiteral("currentStep"), *progress.currentStep);
    if (progress.totalSteps) result.insert(QStringLiteral("totalSteps"), *progress.totalSteps);
    return result;
}

Nukleus::GenerationProgress progressFromJson(const QJsonObject &object)
{
    Nukleus::GenerationProgress result;
    result.stage = stageFromName(object.value(QStringLiteral("stage")).toString());
    result.percentage = object.value(QStringLiteral("percentage")).toInt();
    result.statusText = object.value(QStringLiteral("statusText")).toString();
    if (object.contains(QStringLiteral("currentStep"))) result.currentStep = object.value(QStringLiteral("currentStep")).toInt();
    if (object.contains(QStringLiteral("totalSteps"))) result.totalSteps = object.value(QStringLiteral("totalSteps")).toInt();
    return result;
}
}
