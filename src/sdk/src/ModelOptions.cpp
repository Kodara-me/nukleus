#include "kira/service/ModelOptions.h"
#include <cmath>

namespace KiraService::Protocol
{
QStringList modelSamplers() { return {QString(), "euler", "euler_a", "heun", "dpm2", "dpm++2m", "lcm"}; }
QStringList modelSchedulers()
{
    return {QString(), "discrete", "karras", "exponential", "ays", "sgm_uniform", "simple"};
}
QJsonObject defaultModelOptions(const QString &category)
{
    if (category == "image-generation" || category == "edition")
        return {{"width", 1024}, {"height", 1024},  {"steps", 28},   {"cfgScale", 7.0},
                {"sampler", ""}, {"scheduler", ""}, {"clipSkip", -1}};
    if (category == "llm-chat")
        return {{"temperature", 0.8}, {"topP", 0.95}, {"contextLength", 4096}, {"maxTokens", 1024}};
    if (category == "upscaler")
        return {{"tileSize", 128}};
    if (category == "ip-adapter")
        return {{"strength", 1.0}};
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
    const auto defaults = defaultModelOptions(category);
    if (defaults.isEmpty() || options.keys() != defaults.keys())
        return fail();
    const auto number = [&options](const QString &key, double low, double high, bool integer = false)
    {
        const auto value = options.value(key);
        const double n = value.toDouble();
        return value.isDouble() && std::isfinite(n) && n >= low && n <= high &&
               (!integer || std::floor(n) == n);
    };
    if (category == "image-generation" || category == "edition")
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
    else if (!number("strength", 0, 2))
        return fail();
    return true;
}
} // namespace KiraService::Protocol
