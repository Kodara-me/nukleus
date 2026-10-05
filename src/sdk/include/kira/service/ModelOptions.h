#pragma once
#include <QJsonObject>
#include <QStringList>

namespace KiraService::Protocol
{
QStringList modelSamplers();
QStringList modelSchedulers();
QStringList faceParsingClasses();
QJsonObject defaultModelOptions(const QString &category);
QJsonObject defaultPromptEnhancerOptions(const QString &subtype, bool sampling = false);
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error = nullptr);
} // namespace KiraService::Protocol
