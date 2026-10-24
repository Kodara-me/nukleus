#include "nukleus/ModelOptions.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
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
        if (!defaults.contains(it.key()) && !(category == "image-generation" && it.key() == "clipSkip")
            && category != "animation") return fail();
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
    {
        for (const auto &key : {"vaeModelId", "textEncoderModelId"})
            if (!options.value(key).isString() || options.value(key).toString().isEmpty()) return fail();
        // Any subset of image.animate's parameters may be a model default (sd-cli names are checked by the service).
        auto parameters = options;
        parameters.remove("vaeModelId");
        parameters.remove("textEncoderModelId");
        if (!validateAnimationParameters(parameters)) return fail();
    }
    return true;
}

namespace
{
using Kind = AnimationOptionKind;

AnimationOption option(const QString &key, const QString &group, const QString &flag, Kind kind,
                       double minimum = 0, double maximum = 0, const QStringList &choices = {})
{
    AnimationOption value;
    value.key = key;
    value.group = group;
    value.flag = flag;
    value.kind = kind;
    value.minimum = minimum;
    value.maximum = maximum;
    value.choices = choices;
    return value;
}

AnimationOption helpChoice(const QString &key, const QString &group, const QString &flag, const QString &choicesFlag,
                           Kind kind = Kind::SdCliChoice)
{
    auto value = option(key, group, flag, kind);
    value.choicesFlag = choicesFlag;
    return value;
}

AnimationOption dimension(const QString &key, const QString &flag)
{
    auto value = option(key, "Video", flag, Kind::Integer, 64, 4096);
    value.multipleOf = 16;
    return value;
}

// Free-form values never name a file or a network endpoint: no slash, space or quote can appear.
bool safeText(const QJsonValue &value)
{
    static const QRegularExpression pattern("^[A-Za-z0-9_.,=:&+-]+$");
    return value.isString() && value.toString().toUtf8().size() <= 256 && pattern.match(value.toString()).hasMatch();
}

bool number(const QJsonValue &value, double minimum, double maximum, bool integer = false)
{
    const double n = value.toDouble();
    return value.isDouble() && std::isfinite(n) && n >= minimum && n <= maximum && (!integer || std::floor(n) == n);
}

bool validAnimationValue(const AnimationOption &option, const QJsonValue &value,
                         const QHash<QString, QStringList> *helpChoices)
{
    static const QRegularExpression keyValueList("^[A-Za-z0-9_.+-]+=[A-Za-z0-9_.:&+-]*(,[A-Za-z0-9_.+-]+=[A-Za-z0-9_.:&+-]*)*$");
    static const QRegularExpression binaryMask("^[01](,[01])*$");
    static const QRegularExpression tileSize("^[0-9]+(\\.[0-9]+)?(x[0-9]+(\\.[0-9]+)?)?$");
    static const QRegularExpression sigmaList("^-?[0-9]+(\\.[0-9]+)?(,-?[0-9]+(\\.[0-9]+)?)*$");
    const auto listed = [&](const QString &name) {
        return !helpChoices || helpChoices->value(option.choicesFlag).contains(name);
    };
    switch (option.kind) {
    case Kind::Integer:
        return number(value, option.minimum, option.maximum, true)
            && (option.multipleOf <= 0 || value.toInt() % option.multipleOf == 0)
            // --high-noise-steps takes -1 (automatic split) or a positive count.
            && !(option.key == "highNoiseSteps" && value.toInt() == 0);
    case Kind::Number: return number(value, option.minimum, option.maximum);
    case Kind::Flag:
    case Kind::Boolean: return value.isBool();
    case Kind::Choice:
    case Kind::Container: return value.isString() && option.choices.contains(value.toString());
    case Kind::SdCliChoice: return safeText(value) && listed(value.toString());
    case Kind::Text: return safeText(value);
    case Kind::KeyValueList: return safeText(value) && keyValueList.match(value.toString()).hasMatch();
    case Kind::BinaryMask: return safeText(value) && binaryMask.match(value.toString()).hasMatch();
    case Kind::TileSize: return safeText(value) && tileSize.match(value.toString()).hasMatch();
    case Kind::NumberOrText: return number(value, option.minimum, option.maximum) || safeText(value);
    case Kind::IntegerList: {
        const auto list = value.toArray();
        if (!value.isArray() || list.isEmpty() || list.size() > 64) return false;
        return std::all_of(list.begin(), list.end(),
            [&option](const QJsonValue &item) { return number(item, option.minimum, option.maximum, true); });
    }
    case Kind::Sigmas: {
        static const QRegularExpression presetName("^[A-Za-z0-9_.+-]+$");
        if (!safeText(value)) return false;
        if (!sigmaList.match(value.toString()).hasMatch())
            return presetName.match(value.toString()).hasMatch() && listed(value.toString());
        const auto sigmas = value.toString().split(QLatin1Char(','));
        return std::all_of(sigmas.begin(), sigmas.end(), [](const QString &sigma) { return std::isfinite(sigma.toDouble()); });
    }
    case Kind::LoraWeights: {
        const auto weights = value.toObject();
        if (!value.isObject()) return false;
        for (auto it = weights.begin(); it != weights.end(); ++it)
            if ((it.key() != "lowNoise" && it.key() != "highNoise") || !number(it.value(), option.minimum, option.maximum))
                return false;
        return true;
    }
    }
    return false;
}
} // namespace

const QList<AnimationOption> &animationOptions()
{
    static const QList<AnimationOption> options{
        dimension("width", "-W"),
        dimension("height", "-H"),
        option("videoFrames", "Video", "--video-frames", Kind::Integer, 1, 1024),
        option("fps", "Video", "--fps", Kind::Integer, 1, 120),
        option("container", "Video", {}, Kind::Container, 0, 0, {"webm", "webp", "avi", "mp4"}),
        option("compressionQuality", "Video", "--compression-quality", Kind::Integer, 0, 100),
        option("pingPong", "Video", {}, Kind::Boolean),
        option("steps", "Sampling", "--steps", Kind::Integer, 1, 200),
        option("highNoiseSteps", "Sampling", "--high-noise-steps", Kind::Integer, -1, 200),
        option("cfgScale", "Sampling", "--cfg-scale", Kind::Number, 0, 100),
        option("highNoiseCfgScale", "Sampling", "--high-noise-cfg-scale", Kind::Number, 0, 100),
        option("imgCfgScale", "Sampling", "--img-cfg-scale", Kind::Number, 0, 100),
        option("highNoiseImgCfgScale", "Sampling", "--high-noise-img-cfg-scale", Kind::Number, 0, 100),
        option("guidance", "Sampling", "--guidance", Kind::Number, 0, 100),
        option("highNoiseGuidance", "Sampling", "--high-noise-guidance", Kind::Number, 0, 100),
        helpChoice("sampler", "Sampling", "--sampling-method", "--sampling-method"),
        helpChoice("highNoiseSampler", "Sampling", "--high-noise-sampling-method", "--high-noise-sampling-method"),
        helpChoice("scheduler", "Sampling", "--scheduler", "--scheduler"),
        helpChoice("sigmas", "Sampling", "--sigmas", "--sigmas", Kind::Sigmas),
        option("eta", "Sampling", "--eta", Kind::Number, 0, 10),
        option("highNoiseEta", "Sampling", "--high-noise-eta", Kind::Number, 0, 10),
        option("flowShift", "Sampling", "--flow-shift", Kind::Number, 0, 100),
        option("moeBoundary", "Sampling", "--moe-boundary", Kind::Number, 0, 1),
        option("strength", "Sampling", "--strength", Kind::Number, 0, 1),
        option("slgScale", "Sampling", "--slg-scale", Kind::Number, 0, 100),
        option("highNoiseSlgScale", "Sampling", "--high-noise-slg-scale", Kind::Number, 0, 100),
        option("skipLayers", "Sampling", "--skip-layers", Kind::IntegerList, 0, 255),
        option("highNoiseSkipLayers", "Sampling", "--high-noise-skip-layers", Kind::IntegerList, 0, 255),
        option("skipLayerStart", "Sampling", "--skip-layer-start", Kind::Number, 0, 1),
        option("skipLayerEnd", "Sampling", "--skip-layer-end", Kind::Number, 0, 1),
        option("highNoiseSkipLayerStart", "Sampling", "--high-noise-skip-layer-start", Kind::Number, 0, 1),
        option("highNoiseSkipLayerEnd", "Sampling", "--high-noise-skip-layer-end", Kind::Number, 0, 1),
        option("extraSampleArgs", "Sampling", "--extra-sample-args", Kind::KeyValueList),
        option("rng", "Sampling", "--rng", Kind::Choice, 0, 0, {"std_default", "cuda", "cpu"}),
        option("samplerRng", "Sampling", "--sampler-rng", Kind::Choice, 0, 0, {"std_default", "cuda", "cpu"}),
        helpChoice("prediction", "Sampling", "--prediction", "--prediction"),
        option("loraWeights", "LoRA", {}, Kind::LoraWeights, -10, 10),
        option("loraApplyMode", "LoRA", "--lora-apply-mode", Kind::Choice, 0, 0, {"auto", "immediately", "at_runtime"}),
        option("cacheMode", "Cache", "--cache-mode", Kind::Choice, 0, 0,
               {"easycache", "ucache", "dbcache", "taylorseer", "cache-dit", "spectrum"}),
        option("cacheOption", "Cache", "--cache-option", Kind::KeyValueList),
        option("scmMask", "Cache", "--scm-mask", Kind::BinaryMask),
        option("scmPolicy", "Cache", "--scm-policy", Kind::Choice, 0, 0, {"dynamic", "static"}),
        option("offloadToCpu", "Memory", "--offload-to-cpu", Kind::Flag),
        option("mmap", "Memory", "--mmap", Kind::Flag),
        option("eagerLoad", "Memory", "--eager-load", Kind::Flag),
        option("disablePrefetch", "Memory", "--disable-prefetch", Kind::Flag),
        option("disableSegmentedCompute", "Memory", "--disable-segmented-compute", Kind::Flag),
        option("flashAttention", "Memory", "--flash-attn", Kind::Text),
        option("convDirect", "Memory", "--conv-direct", Kind::Text),
        option("autoFit", "Memory", "--auto-fit", Kind::Choice, 0, 0, {"on", "off"}),
        option("batchedCfg", "Memory", "--batched-cfg", Kind::Choice, 0, 0, {"on", "off"}),
        option("maxVram", "Memory", "--max-vram", Kind::NumberOrText, -1024, 1024),
        option("backend", "Memory", "--backend", Kind::Text),
        option("paramsBackend", "Memory", "--params-backend", Kind::Text),
        option("splitMode", "Memory", "--split-mode", Kind::Text),
        option("threads", "Memory", "-t", Kind::Integer, -1, 1024),
        option("conditioningCacheSize", "Memory", "--conditioning-cache-size", Kind::Integer, 0, 64),
        helpChoice("weightType", "Memory", "--type", "--type"),
        option("vaeFormat", "Memory", "--vae-format", Kind::Choice, 0, 0, {"auto", "flux", "sd3", "flux2", "wan"}),
        option("linearScale", "Memory", "--linear-scale", Kind::Number, 0, 100),
        option("attnScale", "Memory", "--attn-scale", Kind::Number, 0, 100),
        option("vaeTiling", "VAE tiling", "--vae-tiling", Kind::Flag),
        option("temporalTiling", "VAE tiling", "--temporal-tiling", Kind::Flag),
        option("vaeTileSize", "VAE tiling", "--vae-tile-size", Kind::TileSize),
        option("vaeRelativeTileSize", "VAE tiling", "--vae-relative-tile-size", Kind::TileSize),
        option("vaeTileOverlap", "VAE tiling", "--vae-tile-overlap", Kind::Number, 0, 1),
        option("extraTilingArgs", "VAE tiling", "--extra-tiling-args", Kind::KeyValueList),
    };
    return options;
}

QStringList animationRequestOptions() { return {"modelId", "prompt", "negativePrompt", "seed", "hasEndImage", "parameters"}; }

QStringList animationHelpChoiceFlags()
{
    QStringList flags;
    for (const auto &option : animationOptions())
        if (!option.choicesFlag.isEmpty()) flags.append(option.choicesFlag);
    return flags;
}

bool validateAnimationParameters(const QJsonObject &parameters, QString *error,
                                 const QHash<QString, QStringList> *helpChoices)
{
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        const auto &options = animationOptions();
        const auto option = std::find_if(options.cbegin(), options.cend(),
            [&it](const AnimationOption &candidate) { return candidate.key == it.key(); });
        if (option == options.cend()) {
            if (error) *error = QStringLiteral("Unknown animation option: %1").arg(it.key());
            return false;
        }
        if (!validAnimationValue(*option, it.value(), helpChoices)) {
            if (error) *error = QStringLiteral("Invalid value for animation option %1").arg(it.key());
            return false;
        }
    }
    return true;
}
} // namespace Nukleus::Protocol
