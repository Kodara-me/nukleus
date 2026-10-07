#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <cmath>
#include <kira/service/ModelRouting.h>

namespace KiraService::Protocol
{
ModelRouting starterModelRouting()
{
    const auto qualities = [](const QString &prefix) {
        // IDs are persisted selection keys; renaming labels must never change them.
        return QList<ModelQuality>{{prefix + "-low", "Fast", {}, {}, {}, "Rapide"},
                                   {prefix + "-medium", "Balanced", {}, {}, {}, "Équilibré"},
                                   {prefix + "-high", "Quality", {}, {}, {}, "Qualité"}};
    };
    return {{{"photorealism", "Photorealism", {}, {}, qualities("photo"), "Photoréalisme"},
             {"illustration", "Illustration", {}, {}, qualities("illustration"), "Illustration"}},
            qualities("edit")};
}

bool validateModelRouting(const ModelRouting &routing, QString *error)
{
    const auto fixed = starterModelRouting();
    const auto qualitiesValid = [](const QList<ModelQuality> &actual, const QList<ModelQuality> &expected) {
        if (actual.size() != expected.size())
            return false;
        for (int i = 0; i < actual.size(); ++i)
        {
            const auto &q = actual.at(i);
            const auto &label = expected.at(i);
            if (q.id != label.id || q.name != label.name || q.nameFr != label.nameFr ||
                (q.steps && (*q.steps < 1 || *q.steps > 200)) ||
                (q.cfgScale && (!std::isfinite(*q.cfgScale) || *q.cfgScale < 0 || *q.cfgScale > 100)))
                return false;
        }
        return true;
    };
    bool valid = routing.presets.size() == fixed.presets.size() &&
                 qualitiesValid(routing.editingQualities, fixed.editingQualities);
    if (valid)
        for (int i = 0; i < routing.presets.size(); ++i)
        {
            const auto &p = routing.presets.at(i);
            const auto &expected = fixed.presets.at(i);
            if (p.id != expected.id || p.name != expected.name || p.nameFr != expected.nameFr ||
                !qualitiesValid(p.qualities, expected.qualities) || p.positivePrefix.size() > 65536 ||
                p.negativePrefix.size() > 65536)
            {
                valid = false;
                break;
            }
        }
    if (!valid && error)
        *error = QStringLiteral("Presets, qualities and bilingual labels are fixed. Steps must be 1–200, CFG 0–100 and "
                                "prefixes at most 65536 characters.");
    return valid;
}

QJsonObject modelRoutingToJson(const ModelRouting &routing)
{
    const auto qualities = [](const QList<ModelQuality> &list) {
        QJsonArray array;
        for (const auto &q : list)
        {
            QJsonObject item{{"id", q.id}, {"name", q.name}, {"nameFr", q.nameFr}, {"modelId", q.modelId}};
            if (q.steps)
                item.insert("steps", *q.steps);
            if (q.cfgScale)
                item.insert("cfgScale", *q.cfgScale);
            array.append(item);
        }
        return array;
    };
    QJsonArray presets;
    for (const auto &p : routing.presets)
        presets.append(QJsonObject{{"id", p.id},
                                   {"name", p.name},
                                   {"nameFr", p.nameFr},
                                   {"positivePrefix", p.positivePrefix},
                                   {"negativePrefix", p.negativePrefix},
                                   {"qualities", qualities(p.qualities)}});
    return {{"version", 2}, {"presets", presets}, {"editingQualities", qualities(routing.editingQualities)}};
}

std::optional<ModelRouting> modelRoutingFromJson(const QJsonObject &object, QString *error)
{
    const auto fail = [error]() -> std::optional<ModelRouting> {
        if (error)
            *error = QStringLiteral("Malformed model routing configuration");
        return std::nullopt;
    };
    const bool legacy = object.value("version") == QJsonValue(1);
    if ((!legacy && object.value("version") != QJsonValue(2)) || !object.value("presets").isArray() ||
        !object.value("editingQualities").isArray())
        return fail();
    QSet<QString> ids;
    const auto parseQualities = [&](const QJsonValue &value, QList<ModelQuality> &list) {
        if (!value.isArray())
            return false;
        QSet<QString> names;
        for (const auto &item : value.toArray())
        {
            if (!item.isObject())
                return false;
            const auto q = item.toObject();
            if (!q.value("id").isString() || !q.value("name").isString() || !q.value("modelId").isString() ||
                (!legacy && !q.value("nameFr").isString()))
                return false;
            ModelQuality quality{q.value("id").toString(), q.value("name").toString(), q.value("modelId").toString()};
            quality.nameFr = q.value("nameFr").toString();
            if (quality.id.trimmed().isEmpty() || ids.contains(quality.id) || quality.name.trimmed().isEmpty() ||
                names.contains(quality.name.trimmed().toCaseFolded()))
                return false;
            ids.insert(quality.id);
            names.insert(quality.name.trimmed().toCaseFolded());
            if (q.contains("steps"))
            {
                const double number = q.value("steps").toDouble(-1);
                if (!q.value("steps").isDouble() || !std::isfinite(number) || number != std::floor(number) ||
                    number < 1 || number > 200)
                    return false;
                quality.steps = int(number);
            }
            if (q.contains("cfgScale"))
            {
                const double number = q.value("cfgScale").toDouble(-1);
                if (!q.value("cfgScale").isDouble() || !std::isfinite(number) || number < 0 || number > 100)
                    return false;
                quality.cfgScale = number;
            }
            list.append(quality);
        }
        return true;
    };
    ModelRouting routing;
    if (!parseQualities(object.value("editingQualities"), routing.editingQualities))
        return fail();
    QSet<QString> names;
    for (const auto &item : object.value("presets").toArray())
    {
        if (!item.isObject())
            return fail();
        const auto p = item.toObject();
        for (const auto &key : {"id", "name", "positivePrefix", "negativePrefix"})
            if (!p.value(key).isString())
                return fail();
        if (!legacy && !p.value("nameFr").isString())
            return fail();
        GenerationPreset preset{p.value("id").toString(),
                                p.value("name").toString(),
                                p.value("positivePrefix").toString(),
                                p.value("negativePrefix").toString(),
                                {}};
        preset.nameFr = p.value("nameFr").toString();
        if (preset.id.trimmed().isEmpty() || ids.contains(preset.id) || preset.name.trimmed().isEmpty() ||
            names.contains(preset.name.trimmed().toCaseFolded()) || preset.positivePrefix.size() > 65536 ||
            preset.negativePrefix.size() > 65536)
            return fail();
        ids.insert(preset.id);
        names.insert(preset.name.trimmed().toCaseFolded());
        if (!parseQualities(p.value("qualities"), preset.qualities))
            return fail();
        routing.presets.append(preset);
    }
    if (legacy)
    {
        auto fixed = starterModelRouting();
        const auto copyQualities = [](QList<ModelQuality> &target, const QList<ModelQuality> &previous) {
            for (auto &q : target)
                for (const auto &old : previous)
                    if (q.id == old.id)
                    {
                        q.modelId = old.modelId;
                        q.steps = old.steps;
                        q.cfgScale = old.cfgScale;
                        break;
                    }
        };
        for (auto &p : fixed.presets)
            for (const auto &old : routing.presets)
                if (p.id == old.id)
                {
                    p.positivePrefix = old.positivePrefix;
                    p.negativePrefix = old.negativePrefix;
                    copyQualities(p.qualities, old.qualities);
                    break;
                }
        copyQualities(fixed.editingQualities, routing.editingQualities);
        routing = fixed;
    }
    else
    {
        // Accept the previous version-2 labels without rewriting storage on read.
        const auto fixed = starterModelRouting();
        const QStringList previousEnglish{"Low — faster", "Medium", "High — slower"};
        const QStringList previousFrench{"Basse — plus rapide", "Moyenne", "Haute — plus lente"};
        const auto rename = [&](QList<ModelQuality> &qualities, const QList<ModelQuality> &expected) {
            for (auto &q : qualities)
                for (int i = 0; i < expected.size(); ++i)
                    if (q.id == expected.at(i).id && q.name == previousEnglish.at(i)
                        && q.nameFr == previousFrench.at(i)) {
                        q.name = expected.at(i).name;
                        q.nameFr = expected.at(i).nameFr;
                        break;
                    }
        };
        for (auto &preset : routing.presets)
            for (const auto &expected : fixed.presets)
                if (preset.id == expected.id) rename(preset.qualities, expected.qualities);
        rename(routing.editingQualities, fixed.editingQualities);
    }
    if (!validateModelRouting(routing, error))
        return std::nullopt;
    return routing;
}
} // namespace KiraService::Protocol
