#pragma once

#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QMetaType>
#include <QString>
#include <optional>

namespace Nukleus
{
// A model with its saved Steps/CFG snapshot, assigned to a preset or to editing.
struct ModelAssignment
{
    QString modelId;
    std::optional<int> steps = {};
    std::optional<double> cfgScale = {};
    bool operator==(const ModelAssignment &) const = default;
};
struct GenerationPreset
{
    QString id;
    QString name;
    QString positivePrefix;
    QString negativePrefix;
    ModelAssignment model;
    QString nameFr = {};
    // Number of identity portraits (1, 2 or 4) a client requests with this preset.
    int identityViews = 1;
    QString displayLabel(const QLocale &locale = QLocale()) const
    {
        return locale.language() == QLocale::French && !nameFr.isEmpty() ? nameFr : name;
    }
    bool operator==(const GenerationPreset &) const = default;
};
// One of the fixed Fast/Balanced/Quality editing tiers. Exactly one is the default,
// used when a client does not let the user choose a tier.
struct EditingQuality
{
    QString id;
    QString name;
    QString nameFr;
    ModelAssignment model = {};
    bool isDefault = false;
    // Composed first onto the edit instruction and negative prompt sent to sd-cli.
    QString positivePrefix = {};
    QString negativePrefix = {};
    QString displayLabel(const QLocale &locale = QLocale()) const
    {
        return locale.language() == QLocale::French && !nameFr.isEmpty() ? nameFr : name;
    }
    bool operator==(const EditingQuality &) const = default;
};
struct ModelRouting
{
    QList<GenerationPreset> presets;
    QList<EditingQuality> editingQualities;
    const EditingQuality *editingQuality(const QString &id) const
    {
        for (const auto &quality : editingQualities)
            if (quality.id == id)
                return &quality;
        return nullptr;
    }
    const EditingQuality *defaultEditingQuality() const
    {
        for (const auto &quality : editingQualities)
            if (quality.isDefault)
                return &quality;
        return nullptr;
    }
    bool operator==(const ModelRouting &) const = default;
};
namespace Protocol
{
// Generation keeps between one and this many presets.
constexpr int MaximumGenerationPresets = 16;
ModelRouting starterModelRouting();
bool validateModelRouting(const ModelRouting &routing, QString *error = nullptr);
QJsonObject modelRoutingToJson(const ModelRouting &routing);
std::optional<ModelRouting> modelRoutingFromJson(const QJsonObject &object, QString *error = nullptr);
} // namespace Protocol
} // namespace Nukleus
Q_DECLARE_METATYPE(Nukleus::ModelRouting)
