#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>
#include <kira/service/PromptStyle.h>

namespace KiraService
{
enum class ModelCategory
{
    ImageGeneration,
    Upscaler,
    LlmChat,
    Companion,
    Unknown,
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

Q_DECLARE_METATYPE(KiraService::ModelInfo)
Q_DECLARE_METATYPE(KiraService::ModelInstall)
Q_DECLARE_METATYPE(KiraService::ImageGenerationRequest)
Q_DECLARE_METATYPE(KiraService::ImageUpscaleRequest)
Q_DECLARE_METATYPE(KiraService::ImageJobResult)
Q_DECLARE_METATYPE(KiraService::PromptEnhancementRequest)
Q_DECLARE_METATYPE(KiraService::PromptEnhancementResult)
Q_DECLARE_METATYPE(KiraService::TextChatRequest)
Q_DECLARE_METATYPE(KiraService::TextChatResult)
Q_DECLARE_METATYPE(KiraService::ChatModelResult)
Q_DECLARE_METATYPE(KiraService::ServiceCapabilities)
Q_DECLARE_METATYPE(KiraService::ServiceConnectionState)
Q_DECLARE_METATYPE(KiraService::JobState)
