#pragma once

#include "GenerationProgress.h"

#include <QJsonObject>

namespace KiraService::Protocol
{
constexpr int Version = 3;
constexpr quint16 DefaultPort = 9384;
QJsonObject progressToJson(const KiraService::GenerationProgress &progress);
KiraService::GenerationProgress progressFromJson(const QJsonObject &object);
QString stageName(KiraService::GenerationProgressStage stage);
KiraService::GenerationProgressStage stageFromName(const QString &name);
}
