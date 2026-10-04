#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QUrl>

#include <optional>

namespace KiraService
{
enum class ModelCategory
{
    ImageGeneration,
    Upscaler,
    LlmChat,
    IpAdapter,
    Unknown,
    Edition,
    ClipVision,
    PhotoMaker
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
    Edit
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
    double baseImageStrength = 0.55;
    QString modelId;
    QString clipVisionModelId;
    QString ipAdapterModelId;
    QByteArray ipAdapterImageData;
    std::optional<double> ipAdapterStrength;
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
};

struct ServiceCapabilities
{
    bool imageGeneration = false;
    bool imageUpscale = false;
    bool imageEdit = false;
    bool ipAdapter = false;
    bool photoMakerIdentityExport = false;
};

struct PhotoMakerIdentityResult
{
    QString jobId;
    bool succeeded = false;
    QByteArray identityData;
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
Q_DECLARE_METATYPE(KiraService::PhotoMakerIdentityResult)
Q_DECLARE_METATYPE(KiraService::ServiceCapabilities)
Q_DECLARE_METATYPE(KiraService::ServiceConnectionState)
Q_DECLARE_METATYPE(KiraService::JobState)
