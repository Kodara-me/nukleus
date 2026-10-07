#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
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
    QString qualityId;
    // Submit the prompts as the preset's final prompts, bypassing its prompt flow.
    bool skipPromptFlow = false;
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
    // Routed generation: final prompts after the prompt flow, without preset prefixes.
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
    bool promptFlow = false;
};

// Runs a generation preset's prompt flow without generating an image.
struct PromptFlowRequest
{
    QString presetId;
    QString prompt;
    QString negativePrompt;
};

struct PromptFlowResult
{
    QString jobId;
    bool succeeded = false;
    QString prompt;
    QString negativePrompt;
    QString errorCode;
    QString message;
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
Q_DECLARE_METATYPE(KiraService::PromptFlowResult)
Q_DECLARE_METATYPE(KiraService::ServiceCapabilities)
Q_DECLARE_METATYPE(KiraService::ServiceConnectionState)
Q_DECLARE_METATYPE(KiraService::JobState)
