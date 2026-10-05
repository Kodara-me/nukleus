#include "kira/service/ModelOptions.h"
#include <QJsonArray>
#include <QSet>
#include <cmath>

namespace KiraService::Protocol
{
QStringList modelSamplers() { return {QString(), "euler", "euler_a", "heun", "dpm2", "dpm++2m", "lcm"}; }
QStringList modelSchedulers()
{
    return {QString(), "discrete", "karras", "exponential", "ays", "sgm_uniform", "simple"};
}
QJsonObject defaultPromptEnhancerOptions(const QString &subtype, bool sampling)
{
    if (subtype != "T5" && subtype != "LLM" && subtype != "VLM") return {};
    QJsonObject options{{"subtype", subtype}, {"doSample", subtype == "T5" ? sampling : true}};
    if (subtype == "T5") {
        options.insert("maxInputTokens", 256);
        options.insert("maxOutputTokens", 80);
        options.insert("numBeams", 2);
        options.insert("repetitionPenalty", 2.0);
        options.insert("noRepeatNgramSize", 3);
        options.insert("instructionPrompt", "Enhance this prompt (no lora):");
    } else {
        options.insert("contextLength", 4096);
        options.insert("maxOutputTokens", 256);
        options.insert("repetitionPenalty", 1.1);
        options.insert("systemPrompt", "");
        if (subtype == "VLM") options.insert("visionInputEnabled", true);
    }
    if (subtype != "T5" || sampling) {
        options.insert("temperature", 0.2);
        options.insert("topP", 0.9);
        options.insert("topK", 20);
    }
    return options;
}

QStringList faceParsingClasses()
{
    return {"background", "skin", "nose", "eye_g", "l_eye", "r_eye", "l_brow", "r_brow",
            "l_ear", "r_ear", "mouth", "u_lip", "l_lip", "hair", "hat", "ear_r",
            "neck_l", "neck", "cloth"};
}

QJsonObject defaultModelOptions(const QString &category)
{
    if (category == "face-parsing")
        return {{"includedClasses", QJsonArray{"skin", "nose", "l_eye", "r_eye", "l_brow", "r_brow",
                                              "l_ear", "r_ear", "mouth", "u_lip", "l_lip", "hair"}},
                {"dilatePixels", 8}, {"featherPixels", 8}, {"invertMask", false},
                {"preserveAspectRatio", true}, {"outputAtSourceResolution", true},
                {"provider", "CPUExecutionProvider"}};
    if (category == "image-generation" || category == "inpaint" || category == "edition")
        return {{"width", 1024}, {"height", 1024},  {"steps", 28},   {"cfgScale", 7.0},
                {"sampler", ""}, {"scheduler", ""}, {"clipSkip", -1}};
    if (category == "llm-chat")
        return {{"temperature", 0.8}, {"topP", 0.95}, {"contextLength", 4096}, {"maxTokens", 1024}};
    if (category == "upscaler")
        return {{"tileSize", 128}};
    if (category == "ip-adapter" || category == "ip_adapter")
        return {{"strength", 1.0}};
    if (category == "clip_vision")
        return {{"image_size", 224}, {"normalize_input", true}};
    if (category == "photomaker")
        return {{"style_strength", 20}};
    if (category == "prompt-enhancer")
        return defaultPromptEnhancerOptions("LLM");
    return {};
}
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error)
{
    const auto fail = [error]
    {
        if (error)
            *error = QStringLiteral("Invalid recommended options for this model category");
        return false;
    };
    const auto defaults = category == "prompt-enhancer"
        ? defaultPromptEnhancerOptions(options.value("subtype").toString(), options.value("doSample").toBool())
        : defaultModelOptions(category);
    if (defaults.isEmpty() || options.keys() != defaults.keys())
        return fail();
    const auto number = [&options](const QString &key, double low, double high, bool integer = false)
    {
        const auto value = options.value(key);
        const double n = value.toDouble();
        return value.isDouble() && std::isfinite(n) && n >= low && n <= high &&
               (!integer || std::floor(n) == n);
    };
    if (category == "face-parsing") {
        const auto classes = options.value("includedClasses");
        if (!classes.isArray() || classes.toArray().isEmpty()
            || !number("dilatePixels", 0, 256, true) || !number("featherPixels", 0, 256, true)
            || !options.value("invertMask").isBool()
            || !options.value("preserveAspectRatio").isBool()
            || !options.value("outputAtSourceResolution").isBool()
            || options.value("provider") != QJsonValue("CPUExecutionProvider")) return fail();
        QSet<QString> included;
        for (const auto& value : classes.toArray()) {
            if (!value.isString() || !faceParsingClasses().contains(value.toString())
                || included.contains(value.toString())) return fail();
            included.insert(value.toString());
        }
    }
    else if (category == "prompt-enhancer") {
        const auto subtype = options.value("subtype").toString();
        if (!options.value("doSample").isBool() || !number("maxOutputTokens", 1, 131072, true)
            || !number("repetitionPenalty", 0.01, 10)) return fail();
        if (subtype == "T5") {
            if (!number("maxInputTokens", 1, 131072, true) || !number("numBeams", 1, 64, true)
                || !number("noRepeatNgramSize", 0, 128, true)
                || !options.value("instructionPrompt").isString()) return fail();
        } else {
            if (!number("contextLength", 256, 131072, true)
                || options.value("maxOutputTokens").toInt() > options.value("contextLength").toInt()
                || !options.value("systemPrompt").isString()
                || (subtype == "VLM" && !options.value("visionInputEnabled").isBool())) return fail();
        }
        if (subtype != "T5" || options.value("doSample").toBool())
            if (!number("temperature", 0, 2) || !number("topP", 0.01, 1)
                || !number("topK", 0, 10000, true)) return fail();
    }
    else if (category == "image-generation" || category == "inpaint" || category == "edition")
    {
        const int width = options.value("width").toInt();
        const int clip = options.value("clipSkip").toInt();
        if (!number("width", 256, 2048, true) || width % 256 || !number("height", width, width, true) ||
            !number("steps", 1, 200, true) || !number("cfgScale", 0, 100) ||
            !number("clipSkip", -1, 12, true) || clip == 0 || !options.value("sampler").isString() ||
            !modelSamplers().contains(options.value("sampler").toString()) ||
            !options.value("scheduler").isString() ||
            !modelSchedulers().contains(options.value("scheduler").toString()))
            return fail();
    }
    else if (category == "llm-chat")
    {
        if (!number("temperature", 0, 2) || !number("topP", 0, 1) || options.value("topP").toDouble() == 0 ||
            !number("contextLength", 256, 131072, true) ||
            !number("maxTokens", 1, options.value("contextLength").toDouble(), true))
            return fail();
    }
    else if (category == "upscaler")
    {
        if (!number("tileSize", 64, 1024, true) ||
            !QList<int>{64, 128, 256, 512, 1024}.contains(options.value("tileSize").toInt()))
            return fail();
    }
    else if (category == "clip_vision")
    {
        if (!number("image_size", 1, 4096, true) || !options.value("normalize_input").isBool())
            return fail();
    }
    else if (category == "photomaker")
    {
        if (!number("style_strength", 0, 100, true))
            return fail();
    }
    else if (!number("strength", 0, 2))
        return fail();
    return true;
}
} // namespace KiraService::Protocol
