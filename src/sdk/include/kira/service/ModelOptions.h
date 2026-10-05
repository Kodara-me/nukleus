#pragma once
#include <QJsonObject>
#include <QStringList>

namespace KiraService::Protocol
{
QStringList modelSamplers();
QStringList modelSchedulers();
QJsonObject defaultModelOptions(const QString &category);
QJsonObject defaultPromptEnhancerOptions(const QString &subtype, bool sampling = false);
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error = nullptr);
} // namespace KiraService::Protocol
