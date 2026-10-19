#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <nukleus/ModelRouting.h>

namespace Nukleus::Protocol
{
ModelRouting starterModelRouting()
{
    return {{{"photorealism", "Photorealism", {}, {}, {}, "Photoréalisme"},
             {"illustration", "Illustration", {}, {}, {}, "Illustration"}},
            {{"edit-low", "Fast", "Rapide"},
             {"edit-medium", "Balanced", "Équilibré", {}, true},
             {"edit-high", "Quality", "Qualité"}}};
}

bool validateModelRouting(const ModelRouting &routing, QString *error)
{
    const auto labelValid = [](const QString &label) {
        return !label.trimmed().isEmpty() && label.size() <= 256;
    };
    const auto assignmentValid = [](const ModelAssignment &a) {
        return (!a.steps || (*a.steps >= 1 && *a.steps <= 200)) &&
               (!a.cfgScale || (std::isfinite(*a.cfgScale) && *a.cfgScale >= 0 && *a.cfgScale <= 100));
    };
    const auto identityViewsValid = [](int views) { return views == 1 || views == 2 || views == 4; };
    const auto fixedQualities = starterModelRouting().editingQualities;
    bool valid = !routing.presets.isEmpty() && routing.presets.size() <= MaximumGenerationPresets &&
                 routing.editingQualities.size() == fixedQualities.size();
    int defaults = 0;
    for (int i = 0; valid && i < fixedQualities.size(); ++i)
    {
        const auto &q = routing.editingQualities.at(i);
        const auto &expected = fixedQualities.at(i);
        valid = q.id == expected.id && q.name == expected.name && q.nameFr == expected.nameFr &&
                assignmentValid(q.model) && q.positivePrefix.size() <= 65536 && q.negativePrefix.size() <= 65536;
        defaults += q.isDefault ? 1 : 0;
    }
    valid = valid && defaults == 1;
    QSet<QString> presetIds;
    for (const auto &p : routing.presets)
    {
        if (!valid)
            break;
        valid = !p.id.trimmed().isEmpty() && p.id.size() <= 64 && !presetIds.contains(p.id) && labelValid(p.name) &&
                labelValid(p.nameFr) && assignmentValid(p.model) && p.positivePrefix.size() <= 65536 &&
                p.negativePrefix.size() <= 65536 && identityViewsValid(p.identityViews);
        presetIds.insert(p.id);
    }
    if (!valid && error)
        *error = QStringLiteral("Generation needs 1–16 presets with unique IDs and English and French names of at most "
                                "256 characters. Steps must be 1–200, CFG 0–100 and prefixes at most 65536 characters. "
                                "Identity views must be 1, 2 or 4. Editing has the fixed Fast, Balanced and Quality qualities, exactly one of them the default, with "
                                "prefixes of at most 65536 characters.");
    return valid;
}

QJsonObject modelRoutingToJson(const ModelRouting &routing)
{
    const auto assignment = [](const ModelAssignment &a, QJsonObject item) {
        item.insert("modelId", a.modelId);
        if (a.steps)
            item.insert("steps", *a.steps);
        if (a.cfgScale)
            item.insert("cfgScale", *a.cfgScale);
        return item;
    };
    QJsonArray presets;
    for (const auto &p : routing.presets)
        presets.append(assignment(p.model, {{"id", p.id},
                                            {"name", p.name},
                                            {"nameFr", p.nameFr},
                                            {"positivePrefix", p.positivePrefix},
                                            {"negativePrefix", p.negativePrefix},
                                            {"identityViews", p.identityViews}}));
    QJsonArray qualities;
    for (const auto &q : routing.editingQualities)
        qualities.append(assignment(
            q.model, {{"id", q.id},
                      {"name", q.name},
                      {"nameFr", q.nameFr},
                      {"default", q.isDefault},
                      {"positivePrefix", q.positivePrefix},
                      {"negativePrefix", q.negativePrefix}}));
    return {{"version", 6}, {"presets", presets}, {"editingQualities", qualities}};
}

std::optional<ModelRouting> modelRoutingFromJson(const QJsonObject &object, QString *error)
{
    const auto fail = [error]() -> std::optional<ModelRouting> {
        if (error)
            *error = QStringLiteral("Malformed model routing configuration");
        return std::nullopt;
    };
    const int version = object.value("version").toInt();
    if (!object.value("version").isDouble() || version < 1 || version > 6 || !object.value("presets").isArray())
        return fail();
    const bool legacy = version == 1;
    // Versions 1–3 stored Fast/Balanced/Quality tiers; only the Fast tier carries over.
    const bool tiered = version < 4;
    const auto parseAssignment = [](const QJsonObject &o, ModelAssignment &a) {
        if (!o.value("modelId").isString())
            return false;
        a.modelId = o.value("modelId").toString();
        if (o.contains("steps"))
        {
            const double number = o.value("steps").toDouble(-1);
            if (!o.value("steps").isDouble() || !std::isfinite(number) || number != std::floor(number) ||
                number < 1 || number > 200)
                return false;
            a.steps = int(number);
        }
        if (o.contains("cfgScale"))
        {
            const double number = o.value("cfgScale").toDouble(-1);
            if (!o.value("cfgScale").isDouble() || !std::isfinite(number) || number < 0 || number > 100)
                return false;
            a.cfgScale = number;
        }
        return true;
    };
    const auto parseFastTier = [&](const QJsonValue &value, const QString &lowId, ModelAssignment &a) {
        if (!value.isArray())
            return false;
        for (const auto &item : value.toArray())
        {
            if (!item.isObject() || !item.toObject().value("id").isString())
                return false;
            if (item.toObject().value("id").toString() == lowId)
                return parseAssignment(item.toObject(), a);
        }
        return true;
    };
    const auto lowTierId = [](const QString &presetId) {
        return presetId == QStringLiteral("photorealism") ? QStringLiteral("photo-low") : presetId + "-low";
    };
    ModelRouting routing;
    routing.editingQualities = starterModelRouting().editingQualities;
    if (version == 4 || version == 5)
    {
        // Versions 4 and 5 stored one editing assignment; every quality starts with it.
        ModelAssignment editing;
        if (!object.value("editing").isObject() || !parseAssignment(object.value("editing").toObject(), editing))
            return fail();
        for (auto &quality : routing.editingQualities)
            quality.model = editing;
    }
    else
    {
        // Versions 1–3 and 6 store the editing qualities by their fixed IDs. Versions 1–3
        // marked at most one quality permanent, which becomes the default.
        if (!object.value("editingQualities").isArray())
            return fail();
        bool permanent = false;
        for (const auto &value : object.value("editingQualities").toArray())
        {
            const auto q = value.toObject();
            if (!value.isObject() || !q.value("id").isString())
                return fail();
            auto quality = std::find_if(routing.editingQualities.begin(), routing.editingQualities.end(),
                                        [&](const EditingQuality &e) { return e.id == q.value("id").toString(); });
            if (quality == routing.editingQualities.end())
            {
                if (version == 6)
                    return fail();
                continue; // Earlier versions may hold retired custom entries.
            }
            if (!parseAssignment(q, quality->model))
                return fail();
            if (version == 6)
            {
                if (!q.value("default").isBool())
                    return fail();
                quality->isDefault = q.value("default").toBool();
                // Prefixes are optional; earlier version-6 documents predate them.
                for (const auto &key : {"positivePrefix", "negativePrefix"})
                    if (q.contains(key) && !q.value(key).isString())
                        return fail();
                quality->positivePrefix = q.value("positivePrefix").toString();
                quality->negativePrefix = q.value("negativePrefix").toString();
            }
            else if (q.value("permanent").toBool() && !permanent)
            {
                permanent = true;
                for (auto &other : routing.editingQualities)
                    other.isDefault = &other == &*quality;
            }
        }
    }
    QSet<QString> ids;
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
        if (preset.id.trimmed().isEmpty() || ids.contains(preset.id) || preset.positivePrefix.size() > 65536 ||
            preset.negativePrefix.size() > 65536)
            return fail();
        ids.insert(preset.id);
        if (tiered ? !parseFastTier(p.value("qualities"), lowTierId(preset.id), preset.model)
                   : !parseAssignment(p, preset.model))
            return fail();
        // Prompt flows were removed; a promptFlow stored by earlier documents is ignored.
        // Versions 1–4 predate identity views and load with a single view.
        if (version >= 5)
        {
            const double views = p.value("identityViews").toDouble(-1);
            if (!p.value("identityViews").isDouble() || (views != 1 && views != 2 && views != 4))
                return fail();
            preset.identityViews = int(views);
        }
        routing.presets.append(preset);
    }
    if (legacy)
    {
        auto fixed = starterModelRouting();
        for (auto &p : fixed.presets)
            for (const auto &old : routing.presets)
                if (p.id == old.id)
                {
                    p.positivePrefix = old.positivePrefix;
                    p.negativePrefix = old.negativePrefix;
                    p.model = old.model;
                    break;
                }
        fixed.editingQualities = routing.editingQualities;
        routing = fixed;
    }
    if (!validateModelRouting(routing, error))
        return std::nullopt;
    return routing;
}
} // namespace Nukleus::Protocol
