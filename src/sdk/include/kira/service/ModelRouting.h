#pragma once

#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QMetaType>
#include <QString>
#include <optional>

namespace KiraService
{
// A model with its saved Steps/CFG snapshot, assigned to a preset or to editing.
struct ModelAssignment
{
    QString modelId;
    std::optional<int> steps = {};
    std::optional<double> cfgScale = {};
    bool operator==(const ModelAssignment &) const = default;
};
struct PromptFlowStep
{
    QString id;
    QString name;
    QString nameFr;
    QString modelId;
    QString instructions;
    QString displayLabel(const QLocale &locale = QLocale()) const
    {
        return locale.language() == QLocale::French && !nameFr.isEmpty() ? nameFr : name;
    }
    bool operator==(const PromptFlowStep &) const = default;
};
struct GenerationPreset
{
    QString id;
    QString name;
    QString positivePrefix;
    QString negativePrefix;
    ModelAssignment model;
    QString nameFr = {};
    QList<PromptFlowStep> promptFlow = {};
    QString displayLabel(const QLocale &locale = QLocale()) const
    {
        return locale.language() == QLocale::French && !nameFr.isEmpty() ? nameFr : name;
    }
    bool operator==(const GenerationPreset &) const = default;
};
struct ModelRouting
{
    QList<GenerationPreset> presets;
    ModelAssignment editing;
    bool operator==(const ModelRouting &) const = default;
};
namespace Protocol
{
ModelRouting starterModelRouting();
bool validateModelRouting(const ModelRouting &routing, QString *error = nullptr);
QJsonObject modelRoutingToJson(const ModelRouting &routing);
std::optional<ModelRouting> modelRoutingFromJson(const QJsonObject &object, QString *error = nullptr);
} // namespace Protocol
} // namespace KiraService
Q_DECLARE_METATYPE(KiraService::ModelRouting)
