#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>
#include <nukleus/PromptStyle.h>

namespace Nukleus
{
enum class ModelCategory
{
    ImageGeneration,
    Upscaler,
    LlmChat,
    Companion,
    Unknown,
    Animation,
};

struct ModelInfo
{
    QString id;
    QString name;
    QString fileName;
    qint64 fileSize = 0;
    ModelCategory category = ModelCategory::Unknown;
    QString companionFileName;
    QString companionRole;
    QJsonObject recommendedOptions;
    QJsonObject companionFiles;
    QString modelType = QStringLiteral("checkpoint");
    bool explicitContent = false;
    QList<PromptStyle> styles;
    QString displayName;
    QString mmprojModelId;
    // Animation models: roles whose file or shared companion is absent (primary, high-noise, lora-low-noise,
    // lora-high-noise, vae, text-encoder). Such a model cannot run until the files are restored.
    QStringList missingFiles;
    QString displayLabel() const { return displayName.trimmed().isEmpty() ? fileName : displayName.trimmed(); }
    // An image-generation model's prompt format (natural, phrases or tags); phrases when unset.
    QString promptFormat() const { return recommendedOptions.value("promptFormat").toString(QStringLiteral("phrases")); }
};

enum class ModelInstallState
{
    Requesting,
    Accepted,
    Queued,
    Downloading,
    Verifying,
    Completed,
    Failed,
    Canceled
};

struct ModelInstall
{
    QString installId;
    ModelCategory category = ModelCategory::Unknown;
    QUrl url;
    QUrl companionUrl;
    ModelInstallState state = ModelInstallState::Requesting;
    qint64 bytesDownloaded = 0;
    std::optional<qint64> totalBytes;
    QString message;
};

enum class ImageOperation
{
    Generate,
    Upscale,
    Edit,
};

struct ImageGenerationRequest
{
    QString prompt;
    QString negativePrompt;
    QString sampler;
    QString scheduler;
    int width = 1024;
    int height = 1024;
    int steps = 28;
    double cfgScale = 7.0;
    int clipSkip = -1;
    int seed = -1;
    QByteArray baseImageData;
    QList<QByteArray> editImages;
    // Generation conditioning images (1–8), sent in order; exclusive with baseImageData.
    QList<QByteArray> referenceImages;
    double baseImageStrength = 0.55;
    QString modelId;
    QString styleId;
    QString presetId;
    // Editing quality tier (edit-low/edit-medium/edit-high); edits only.
    QString qualityId;
};

// An image.animate request: a short video from a start image, and optionally an end image.
struct AnimationRequest
{
    QString modelId; // An installed animation model.
    QString prompt;  // Nonblank; LoRA tags are not accepted (see loraWeights).
    QString negativePrompt;
    int seed = -1; // -1 picks a random seed.
    QByteArray startImage; // PNG or JPEG.
    QByteArray endImage;   // Optional PNG or JPEG.
    // Options of image.animate's table (Nukleus::Protocol::animationOptions()); each one left out uses
    // the model's default, else sd-cli's own.
    QJsonObject parameters;
};

struct AnimationResult
{
    QString jobId;
    bool succeeded = false;
    QByteArray videoData;
    QString mimeType; // video/webm, image/webp or video/x-msvideo.
    int frames = 0;
    int fps = 0;
    QString errorCode;
    QString message;
};

struct ImageUpscaleRequest
{
    QByteArray sourceImageData;
    QString modelId;
};

struct ImageJobResult
{
    QString jobId;
    ImageOperation operation = ImageOperation::Generate;
    bool succeeded = false;
    QByteArray imageData;
    QString mimeType;
    QString errorCode;
    QString message;
    // Routed generation: the submitted prompts, without preset prefixes.
    QString prompt = {};
    QString negativePrompt = {};
};

struct ServiceCapabilities
{
    bool imageGeneration = false;
    bool imageUpscale = false;
    bool imageEdit = false;
    bool promptEnhancement = false;
    bool modelRouting = false;
    bool textChat = false;
    QStringList textChatEngines; // Ready text.chat engines: "server", "completion".
    bool chatModelControl = false; // text.chat.load and text.chat.unload.
    bool textChatImages = false; // text.chat accepts images for LLMs with an MMPROJ.
    bool imageAnimate = false; // image.animate turns an image into a video with an animation model.
};

// Result of text.chat.load (load true) or text.chat.unload (load false).
struct ChatModelResult
{
    QString jobId;
    bool load = true;
    bool succeeded = false;
    bool coldStart = false; // load: the request loaded the model; false when it was already loaded.
    bool unloaded = false; // unload: a model was loaded and has been unloaded.
    qint64 durationMs = -1;
    QString errorCode;
    QString message;
};

struct TextChatMessage
{
    QString role; // "system" (first only), "user" or "assistant".
    QString content;
};

struct TextChatRequest
{
    QString modelId;
    QList<TextChatMessage> messages;
    QJsonObject parameters; // Optional sampling, engine and memory-placement overrides.
    // Up to eight PNG or JPEG images shown to the model, in order, before the last user message's
    // text. Requires the server engine and an LLM with an MMPROJ (ServiceClient::canChatWithImages).
    QList<QByteArray> images;
};

struct TextChatResult
{
    QString jobId;
    bool succeeded = false;
    QString text;
    QString engine;
    QString finishReason; // Empty when the engine does not report it.
    int promptTokens = -1; // -1 when the engine does not report usage.
    int completionTokens = -1;
    qint64 durationMs = -1; // Whole turn measured by the service, model loading included.
    bool coldStart = false; // The turn loaded the model.
    QString errorCode;
    QString message;
    QJsonValue json; // The parsed reply when the request set jsonSchema; Undefined otherwise.
};

struct PromptEnhancementRequest
{
    QString modelId;
    QString prompt;
    QJsonObject parameters; // Optional overrides of the selected model's LLM options.
};

struct PromptEnhancementResult
{
    QString jobId;
    bool succeeded = false;
    QString prompt;
    QString errorCode;
    QString message;
};

enum class ServiceConnectionState
{
    Disconnected,
    Connecting,
    Authenticated,
    Ready,
    Error
};

enum class JobState
{
    Uploading,
    Queued,
    Running,
    Completed,
    Failed
};
}

Q_DECLARE_METATYPE(Nukleus::ModelInfo)
Q_DECLARE_METATYPE(Nukleus::ModelInstall)
Q_DECLARE_METATYPE(Nukleus::ImageGenerationRequest)
Q_DECLARE_METATYPE(Nukleus::ImageUpscaleRequest)
Q_DECLARE_METATYPE(Nukleus::ImageJobResult)
Q_DECLARE_METATYPE(Nukleus::AnimationRequest)
Q_DECLARE_METATYPE(Nukleus::AnimationResult)
Q_DECLARE_METATYPE(Nukleus::PromptEnhancementRequest)
Q_DECLARE_METATYPE(Nukleus::PromptEnhancementResult)
Q_DECLARE_METATYPE(Nukleus::TextChatRequest)
Q_DECLARE_METATYPE(Nukleus::TextChatResult)
Q_DECLARE_METATYPE(Nukleus::ChatModelResult)
Q_DECLARE_METATYPE(Nukleus::ServiceCapabilities)
Q_DECLARE_METATYPE(Nukleus::ServiceConnectionState)
Q_DECLARE_METATYPE(Nukleus::JobState)
