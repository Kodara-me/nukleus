#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <kira/service/ModelRouting.h>

namespace KiraService::Protocol
{
ModelRouting starterModelRouting()
{
    return {{{"photorealism", "Photorealism", {}, {}, {}, "Photoréalisme"},
             {"illustration", "Illustration", {}, {}, {}, "Illustration"}},
            {}};
}

bool validateModelRouting(const ModelRouting &routing, QString *error)
{
    const auto fixed = starterModelRouting();
    const auto assignmentValid = [](const ModelAssignment &a) {
        return (!a.steps || (*a.steps >= 1 && *a.steps <= 200)) &&
               (!a.cfgScale || (std::isfinite(*a.cfgScale) && *a.cfgScale >= 0 && *a.cfgScale <= 100));
    };
    const auto flowValid = [](const QList<PromptFlowStep> &steps) {
        if (steps.size() > 4)
            return false;
        QSet<QString> ids;
        for (const auto &step : steps)
        {
            if (step.id.trimmed().isEmpty() || ids.contains(step.id) || step.name.trimmed().isEmpty() ||
                step.nameFr.trimmed().isEmpty() || step.instructions.size() > 65536)
                return false;
            ids.insert(step.id);
        }
        return true;
    };
    const auto identityViewsValid = [](int views) { return views == 1 || views == 2 || views == 4; };
    bool valid = routing.presets.size() == fixed.presets.size() && assignmentValid(routing.editing);
    if (valid)
        for (int i = 0; i < routing.presets.size(); ++i)
        {
            const auto &p = routing.presets.at(i);
            const auto &expected = fixed.presets.at(i);
            if (p.id != expected.id || p.name != expected.name || p.nameFr != expected.nameFr ||
                !assignmentValid(p.model) || p.positivePrefix.size() > 65536 || p.negativePrefix.size() > 65536 ||
                !flowValid(p.promptFlow) || !identityViewsValid(p.identityViews))
            {
                valid = false;
                break;
            }
        }
    if (!valid && error)
        *error = QStringLiteral("Presets and bilingual labels are fixed. Steps must be 1–200, CFG 0–100 and "
                                "prefixes at most 65536 characters. Identity views must be 1, 2 or 4. Prompt flows allow at most 4 steps with unique "
                                "IDs, bilingual names and instructions of at most 65536 characters.");
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
    {
        QJsonArray flow;
        for (const auto &step : p.promptFlow)
            flow.append(QJsonObject{{"id", step.id},
                                    {"name", step.name},
                                    {"nameFr", step.nameFr},
                                    {"modelId", step.modelId},
                                    {"instructions", step.instructions}});
        presets.append(assignment(p.model, {{"id", p.id},
                                            {"name", p.name},
                                            {"nameFr", p.nameFr},
                                            {"positivePrefix", p.positivePrefix},
                                            {"negativePrefix", p.negativePrefix},
                                            {"promptFlow", flow},
                                            {"identityViews", p.identityViews}}));
    }
    return {{"version", 5}, {"presets", presets}, {"editing", assignment(routing.editing, {})}};
}

std::optional<ModelRouting> modelRoutingFromJson(const QJsonObject &object, QString *error)
{
    const auto fail = [error]() -> std::optional<ModelRouting> {
        if (error)
            *error = QStringLiteral("Malformed model routing configuration");
        return std::nullopt;
    };
    const int version = object.value("version").toInt();
    if (!object.value("version").isDouble() || version < 1 || version > 5 || !object.value("presets").isArray())
        return fail();
    const bool legacy = version == 1;
    // Versions 1–3 stored Fast/Balanced/Quality tiers; only the Fast tier carries over.
    const bool tiered = version < 4;
    const bool flows = version >= 3;
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
    if (tiered ? !parseFastTier(object.value("editingQualities"), "edit-low", routing.editing)
               : (!object.value("editing").isObject() ||
                  !parseAssignment(object.value("editing").toObject(), routing.editing)))
        return fail();
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
        // Versions 1 and 2 predate prompt flows and load with an empty flow.
        if (flows)
        {
            if (!p.value("promptFlow").isArray())
                return fail();
            for (const auto &value : p.value("promptFlow").toArray())
            {
                if (!value.isObject())
                    return fail();
                const auto s = value.toObject();
                for (const auto &key : {"id", "name", "nameFr", "modelId", "instructions"})
                    if (!s.value(key).isString())
                        return fail();
                preset.promptFlow.append({s.value("id").toString(), s.value("name").toString(),
                                          s.value("nameFr").toString(), s.value("modelId").toString(),
                                          s.value("instructions").toString()});
            }
        }
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
        fixed.editing = routing.editing;
        routing = fixed;
    }
    if (!validateModelRouting(routing, error))
        return std::nullopt;
    return routing;
}
} // namespace KiraService::Protocol
