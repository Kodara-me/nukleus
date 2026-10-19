#pragma once
#include <QJsonObject>
#include <QStringList>

namespace Nukleus::Protocol
{
QStringList modelSamplers();
QStringList modelSchedulers();
// How an image model reads prompts: natural sentences (Flux), short comma-separated phrases (SDXL,
// the default) or Danbooru-style tags (Illustrious). Clients adapt prompt writing to it.
QStringList modelPromptFormats();
QJsonObject defaultModelOptions(const QString &category);
// LLM option keys that at most one installed llm-chat model may set to true.
QStringList exclusiveLlmRoles();
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error = nullptr);
} // namespace Nukleus::Protocol
