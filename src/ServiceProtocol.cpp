#include "ServiceProtocol.h"

#include <QHash>

namespace ServiceProtocol
{
QString stageName(GenerationProgressStage stage)
{
    switch (stage) {
    case GenerationProgressStage::Preparing: return QStringLiteral("preparing");
    case GenerationProgressStage::Starting: return QStringLiteral("starting");
    case GenerationProgressStage::LoadingModel: return QStringLiteral("loading-model");
    case GenerationProgressStage::Sampling: return QStringLiteral("sampling");
    case GenerationProgressStage::Decoding: return QStringLiteral("decoding");
    case GenerationProgressStage::Saving: return QStringLiteral("saving");
    case GenerationProgressStage::Complete: return QStringLiteral("complete");
    case GenerationProgressStage::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

GenerationProgressStage stageFromName(const QString &name)
{
    static const QHash<QString, GenerationProgressStage> stages = {
        {QStringLiteral("preparing"), GenerationProgressStage::Preparing},
        {QStringLiteral("starting"), GenerationProgressStage::Starting},
        {QStringLiteral("loading-model"), GenerationProgressStage::LoadingModel},
        {QStringLiteral("sampling"), GenerationProgressStage::Sampling},
        {QStringLiteral("decoding"), GenerationProgressStage::Decoding},
        {QStringLiteral("saving"), GenerationProgressStage::Saving},
        {QStringLiteral("complete"), GenerationProgressStage::Complete},
        {QStringLiteral("failed"), GenerationProgressStage::Failed}};
    return stages.value(name, GenerationProgressStage::Failed);
}

QJsonObject progressToJson(const GenerationProgress &progress)
{
    QJsonObject result{{QStringLiteral("stage"), stageName(progress.stage)},
                       {QStringLiteral("percentage"), progress.percentage},
                       {QStringLiteral("statusText"), progress.statusText}};
    if (progress.currentStep) result.insert(QStringLiteral("currentStep"), *progress.currentStep);
    if (progress.totalSteps) result.insert(QStringLiteral("totalSteps"), *progress.totalSteps);
    return result;
}

GenerationProgress progressFromJson(const QJsonObject &object)
{
    GenerationProgress result;
    result.stage = stageFromName(object.value(QStringLiteral("stage")).toString());
    result.percentage = object.value(QStringLiteral("percentage")).toInt();
    result.statusText = object.value(QStringLiteral("statusText")).toString();
    if (object.contains(QStringLiteral("currentStep"))) result.currentStep = object.value(QStringLiteral("currentStep")).toInt();
    if (object.contains(QStringLiteral("totalSteps"))) result.totalSteps = object.value(QStringLiteral("totalSteps")).toInt();
    return result;
}
}
