#include "kira/service/ServiceProtocol.h"

#include <QHash>

namespace KiraService::Protocol
{
QString stageName(KiraService::GenerationProgressStage stage)
{
    switch (stage) {
    case KiraService::GenerationProgressStage::Preparing: return QStringLiteral("preparing");
    case KiraService::GenerationProgressStage::EnhancingPrompt: return QStringLiteral("enhancing-prompt");
    case KiraService::GenerationProgressStage::Starting: return QStringLiteral("starting");
    case KiraService::GenerationProgressStage::LoadingModel: return QStringLiteral("loading-model");
    case KiraService::GenerationProgressStage::Sampling: return QStringLiteral("sampling");
    case KiraService::GenerationProgressStage::Decoding: return QStringLiteral("decoding");
    case KiraService::GenerationProgressStage::Saving: return QStringLiteral("saving");
    case KiraService::GenerationProgressStage::Complete: return QStringLiteral("complete");
    case KiraService::GenerationProgressStage::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

KiraService::GenerationProgressStage stageFromName(const QString &name)
{
    static const QHash<QString, KiraService::GenerationProgressStage> stages = {
        {QStringLiteral("preparing"), KiraService::GenerationProgressStage::Preparing},
        {QStringLiteral("enhancing-prompt"), KiraService::GenerationProgressStage::EnhancingPrompt},
        {QStringLiteral("starting"), KiraService::GenerationProgressStage::Starting},
        {QStringLiteral("loading-model"), KiraService::GenerationProgressStage::LoadingModel},
        {QStringLiteral("sampling"), KiraService::GenerationProgressStage::Sampling},
        {QStringLiteral("decoding"), KiraService::GenerationProgressStage::Decoding},
        {QStringLiteral("saving"), KiraService::GenerationProgressStage::Saving},
        {QStringLiteral("complete"), KiraService::GenerationProgressStage::Complete},
        {QStringLiteral("failed"), KiraService::GenerationProgressStage::Failed}};
    return stages.value(name, KiraService::GenerationProgressStage::Failed);
}

QJsonObject progressToJson(const KiraService::GenerationProgress &progress)
{
    QJsonObject result{{QStringLiteral("stage"), stageName(progress.stage)},
                       {QStringLiteral("percentage"), progress.percentage},
                       {QStringLiteral("statusText"), progress.statusText}};
    if (progress.currentStep) result.insert(QStringLiteral("currentStep"), *progress.currentStep);
    if (progress.totalSteps) result.insert(QStringLiteral("totalSteps"), *progress.totalSteps);
    return result;
}

KiraService::GenerationProgress progressFromJson(const QJsonObject &object)
{
    KiraService::GenerationProgress result;
    result.stage = stageFromName(object.value(QStringLiteral("stage")).toString());
    result.percentage = object.value(QStringLiteral("percentage")).toInt();
    result.statusText = object.value(QStringLiteral("statusText")).toString();
    if (object.contains(QStringLiteral("currentStep"))) result.currentStep = object.value(QStringLiteral("currentStep")).toInt();
    if (object.contains(QStringLiteral("totalSteps"))) result.totalSteps = object.value(QStringLiteral("totalSteps")).toInt();
    return result;
}
}
