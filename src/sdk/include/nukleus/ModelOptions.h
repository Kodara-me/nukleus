#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace Nukleus::Protocol
{
QStringList modelSamplers();
QStringList modelSchedulers();
// How an image model reads prompts: natural sentences (Flux), short comma-separated phrases (SDXL,
// the default) or Danbooru-style tags (Illustrious). Clients adapt prompt writing to it.
QStringList modelPromptFormats();
QJsonObject defaultModelOptions(const QString &category);
// LLM option keys that at most one installed llm-chat model may set to true.
QStringList exclusiveLlmRoles();
bool validateModelOptions(const QString &category, const QJsonObject &options, QString *error = nullptr);

// How an image.animate option's value is checked and passed to sd-cli.
enum class AnimationOptionKind
{
    Integer,      // A whole number within [minimum, maximum], a multiple of multipleOf when set.
    Number,       // A finite number within [minimum, maximum].
    Flag,         // A boolean; the sd-cli flag is passed alone when true.
    Boolean,      // A boolean handled by the service (no sd-cli flag).
    Choice,       // One of choices.
    SdCliChoice,  // A name listed by sd-cli's --help for choicesFlag.
    Text,         // A free-form value of safe characters (assignments, device names).
    KeyValueList, // Comma-separated key=value pairs of safe characters.
    BinaryMask,   // Comma-separated 0/1 values.
    TileSize,     // "S" or "WxH", each a non-negative number.
    NumberOrText, // A finite number within [minimum, maximum], or a Text value.
    IntegerList,  // An array of 1–64 integers within [minimum, maximum], passed as "[a,b,c]".
    Sigmas,       // Comma-separated finite numbers, or a preset listed by sd-cli's --help for choicesFlag.
    LoraWeights,  // An object with optional lowNoise and highNoise numbers within [minimum, maximum].
    Container,    // The output container, one of choices; it names the output file.
};

// One row of image.animate's option table: request parameters and animation model defaults share it.
struct AnimationOption
{
    QString key;
    QString group; // Video, Sampling, LoRA, Cache, Memory or VAE tiling.
    QString flag;  // The sd-cli flag; empty when the service handles the option itself.
    AnimationOptionKind kind = AnimationOptionKind::Text;
    double minimum = 0;
    double maximum = 0;
    int multipleOf = 0;
    QStringList choices;
    QString choicesFlag; // The sd-cli option whose --help text lists the accepted names.
};

// Every option of options.parameters, in display order. Each may also be an animation model default.
const QList<AnimationOption> &animationOptions();
// The top-level options of an image.animate request, outside options.parameters.
QStringList animationRequestOptions();
// The sd-cli options whose --help text lists accepted names, for SdCliChoice and Sigmas options.
QStringList animationHelpChoiceFlags();
// Validates image.animate parameters, from a request or a model's defaults: known keys, types and ranges.
// With helpChoices (sd-cli flag -> names listed by its --help), names are also checked against them;
// without, they are only checked for safe characters.
bool validateAnimationParameters(const QJsonObject &parameters, QString *error = nullptr,
                                 const QHash<QString, QStringList> *helpChoices = nullptr);
} // namespace Nukleus::Protocol
