#include "nukleus/ModelOptions.h"
#include <cmath>

namespace Nukleus::Protocol
{
QStringList modelSamplers() { return {QString(), "euler", "euler_a", "heun", "dpm2", "dpm++2m", "lcm"}; }
QStringList modelSchedulers()
{
    return {QString(), "discrete", "karras", "exponential", "ays", "sgm_uniform", "simple"};
}
QStringList modelPromptFormats() { return {"natural", "phrases", "tags"}; }
QJsonObject defaultModelOptions(const QString &category)
{
    if (category == "image-generation")
        return {{"width", 1024}, {"height", 1024},  {"steps", 28},   {"cfgScale", 7.0},
                {"sampler", ""}, {"scheduler", ""}, {"canEdit", true}, {"canGenerate", true},
                {"vaeModelId", ""}, {"textEncoderModelId", ""}, {"promptFormat", "phrases"}};
    if (category == "llm-chat")
        return {{"temperature", 0.8}, {"topP", 0.95}, {"contextLength", 4096}, {"maxTokens", 1024}, {"canBeTextEncoder", false},
                {"defaultPromptEnhancer", false}, {"defaultPromptTranslator", false}, {"defaultImageDescriber", false}};
    if (category == "upscaler")
        return {};
    if (category == "companion")
        return {{"subtype", "vae"}};
    // Both IDs are required: an animation model names its installed VAE and T5-XXL text-encoder companions.
    if (category == "animation")
        return {{"vaeModelId", ""}, {"textEncoderModelId", ""}};
    return {};
}
QStringList exclusiveLlmRoles() { return {"defaultPromptEnhancer", "defaultPromptTranslator", "defaultImageDescriber"}; }
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error)
{
    const auto fail = [error]
    {
        if (error)
            *error = QStringLiteral("Invalid recommended options for this model category");
        return false;
    };
    const auto defaults = defaultModelOptions(category);
    if (category == "upscaler") return options.isEmpty() || fail();
    if (defaults.isEmpty()) return fail();
    auto required = defaults;
    if (category == "image-generation")
        for (const auto &key : {"canEdit", "canGenerate", "vaeModelId", "textEncoderModelId", "promptFormat"}) required.remove(key);
    if (category == "llm-chat")
        for (const auto &key : QStringList{"canBeTextEncoder"} + exclusiveLlmRoles()) required.remove(key);
    for (auto it = required.begin(); it != required.end(); ++it)
        if (!options.contains(it.key())) return fail();
    for (auto it = options.begin(); it != options.end(); ++it)
        if (!defaults.contains(it.key()) && !(category == "image-generation" && it.key() == "clipSkip")) return fail();
    const auto number = [&options](const QString &key, double low, double high, bool integer = false)
    {
        const auto value = options.value(key);
        const double n = value.toDouble();
        return value.isDouble() && std::isfinite(n) && n >= low && n <= high &&
               (!integer || std::floor(n) == n);
    };
    if (category == "image-generation")
    {
        const int width = options.value("width").toInt();
        for (const auto &key : {"canEdit", "canGenerate"})
            if (options.contains(key) && !options.value(key).isBool()) return fail();
        if (!options.value("canEdit").toBool(true) && !options.value("canGenerate").toBool(true)) return fail();
        for (const auto &key : {"vaeModelId", "textEncoderModelId"})
            if (options.contains(key) && !options.value(key).isString()) return fail();
        if (options.contains("promptFormat") && !modelPromptFormats().contains(options.value("promptFormat").toString()))
            return fail();
        if (!number("width", 256, 2048, true) || width % 256 || !number("height", width, width, true) ||
            !number("steps", 1, 200, true) || !number("cfgScale", 0, 100) ||
            (options.contains("clipSkip") && (!number("clipSkip", -1, 12, true) || options.value("clipSkip").toInt() == 0)) || !options.value("sampler").isString() ||
            !modelSamplers().contains(options.value("sampler").toString()) ||
            !options.value("scheduler").isString() ||
            !modelSchedulers().contains(options.value("scheduler").toString()))
            return fail();
    }
    else if (category == "llm-chat")
    {
        for (const auto &key : QStringList{"canBeTextEncoder"} + exclusiveLlmRoles())
            if (options.contains(key) && !options.value(key).isBool()) return fail();
        if (!number("temperature", 0, 2) || !number("topP", 0, 1) || options.value("topP").toDouble() == 0 ||
            !number("contextLength", 256, 131072, true) ||
            !number("maxTokens", 1, options.value("contextLength").toDouble(), true))
            return fail();
    }
    else if (category == "companion" && !QStringList{"vae", "mmproj", "t5xxl"}.contains(options.value("subtype").toString()))
        return fail();
    else if (category == "animation")
        for (const auto &key : {"vaeModelId", "textEncoderModelId"})
            if (!options.value(key).isString() || options.value(key).toString().isEmpty()) return fail();
    return true;
}
} // namespace Nukleus::Protocol
