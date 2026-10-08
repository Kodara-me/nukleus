#pragma once
#include <QJsonObject>
#include <QStringList>

namespace KiraService::Protocol
{
QStringList modelSamplers();
QStringList modelSchedulers();
QJsonObject defaultModelOptions(const QString &category);
// LLM option keys that at most one installed llm-chat model may set to true.
QStringList exclusiveLlmRoles();
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error = nullptr);
} // namespace KiraService::Protocol
