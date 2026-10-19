#pragma once

#include "GenerationProgress.h"

#include <QJsonObject>

namespace Nukleus::Protocol
{
constexpr int Version = 3;
constexpr quint16 DefaultPort = 9384;
QJsonObject progressToJson(const Nukleus::GenerationProgress &progress);
Nukleus::GenerationProgress progressFromJson(const QJsonObject &object);
QString stageName(Nukleus::GenerationProgressStage stage);
Nukleus::GenerationProgressStage stageFromName(const QString &name);
}
