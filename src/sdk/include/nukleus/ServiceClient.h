#pragma once
#include "GenerationProgress.h"
#include "ImageTypes.h"
#include "ModelRouting.h"
#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QUrl>
class QJsonObject;
class QWebSocket;
namespace Nukleus
{
class ServiceClient final : public QObject
{
    Q_OBJECT
public:
    explicit ServiceClient(QObject *parent = nullptr);
    QUrl endpoint() const;
    QString token() const;
    void setConfiguration(const QUrl &endpoint, const QString &token);
    void connectToService();
    void disconnectFromService();
    bool isAuthenticated() const;
    ServiceCapabilities capabilities() const;
    bool supports(ImageOperation operation) const;
    bool canSubmit(ImageOperation operation, const QString &modelId) const;
    bool modelListAvailable() const;
    QList<ModelInfo> models(ModelCategory category = ModelCategory::Unknown) const;
    // Opaque ID of the listed LLM marked as the service-wide default for this role, or empty.
    QString defaultPromptEnhancerModelId() const;
    QString defaultPromptTranslatorModelId() const;
    QString defaultImageDescriberModelId() const;
    QList<ModelInstall> installs() const;
    void requestModels();
    void requestModelRouting();
    bool configureModelRouting(const ModelRouting &routing);
    std::optional<ModelRouting> modelRouting() const;
    QString installModel(ModelCategory category, const QUrl &url, const QUrl &companionUrl = {}, const QJsonObject &recommendedOptions = {}, const QJsonObject &companionUrls = {},
        const QString &modelType = QStringLiteral("checkpoint"), const QString &existingModelId = {}, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {}, std::optional<QString> mmprojModelId = {});
    bool configureModel(const QString &id, const QJsonObject &options, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {}, std::optional<QString> mmprojModelId = {});
    bool configureModelStyles(const QString &id, const QList<PromptStyle> &styles);
    bool cancelInstall(const QString &installId);
    bool deleteModel(const QString &id);
    bool deleteCompanion(const QString& id, const QString& role);
    QString submitEdit(const ImageGenerationRequest &request);
    QString submitGeneration(const ImageGenerationRequest &request);
    QString submitUpscale(const ImageUpscaleRequest &request);
    bool canEnhancePrompt(const QString& modelId) const;
    QString enhancePrompt(const PromptEnhancementRequest& request);
    bool canChat(const QString& modelId) const;
    // The model can chat and has an MMPROJ vision projector, so TextChatRequest::images can be sent.
    bool canChatWithImages(const QString& modelId) const;
    static constexpr int MaximumChatImages = 8;
    QString submitChat(const TextChatRequest& request);
    // Preloads the server engine with the same model and parameters as later chat requests.
    QString loadChatModel(const QString& modelId, const QJsonObject& parameters = {});
    QString unloadChatModel();
    // The service offers image.animate.
    bool canAnimate() const;
    // The service offers image.animate's pingPong parameter, which returns a forward-then-backward loop.
    bool canPingPong() const;
    // The service offers image.animate's mp4 container (H.264, yuv420p, fast start), which plays everywhere.
    bool canAnimateMp4() const;
    // Submits an animation with an installed animation model and its start image (and end image, if any);
    // progress arrives through generationProgressChanged and the video through animationFinished.
    QString submitAnimation(const AnimationRequest &request);
signals:
    void serviceStateChanged(Nukleus::ServiceConnectionState state, const QString &code, const QString &detail);
    void capabilitiesChanged(const Nukleus::ServiceCapabilities &capabilities);
    void modelsChanged();
    void modelRoutingChanged();
    void modelRoutingConfigured();
    void modelRoutingFailed(const QString &message);
    void installsChanged();
    void modelConfigured(const QString &id, const QJsonObject &options);
    void modelStylesConfigured(const QString &id, const QList<Nukleus::PromptStyle> &styles);
    void modelDeleted(const QString &id);
    void companionDeleted(const QString& id, const QString& role);
    void modelOperationFailed(const QString &id, const QString &message);
    void jobStateChanged(const QString &jobId, Nukleus::JobState state, int queuePosition);
    void jobOutput(const QString &jobId, const QString &stream, const QString &text);
    void generationProgressChanged(const QString &jobId, const Nukleus::GenerationProgress &progress);
    void imageJobFinished(const Nukleus::ImageJobResult &result);
    void promptEnhancementFinished(const Nukleus::PromptEnhancementResult& result);
    void chatFinished(const Nukleus::TextChatResult& result);
    void chatModelFinished(const Nukleus::ChatModelResult& result);
    void animationFinished(const Nukleus::AnimationResult &result);
private:
    QString defaultLlmModelId(const QString &role) const;
    struct PendingJob
    {
        ImageOperation operation = ImageOperation::Generate;
        QList<QByteArray> inputImages;
        QString mimeType;
        bool promptEnhancement = false;
        bool textChat = false;
        QString chatModelOperation; // "text.chat.load" or "text.chat.unload".
        QString prompt;
        QString negativePrompt;
        bool animation = false;
        int frames = 0;
        int fps = 0;
    };
    QString submit(ImageOperation operation, const QJsonObject& options, const QList<QByteArray>& inputImages);
    void authenticate();
    void handleTextMessage(const QString &message);
    void handleBinaryMessage(const QByteArray &data);
    void updateCapabilities(const ServiceCapabilities &capabilities);
    void clearCatalog();
    ModelInstall *findInstall(const QString &installId);
    void failActiveInstalls(const QString &message);
    void failJob(const QString &jobId, const QString &errorCode, const QString &message);
    void failAllJobs(const QString &errorCode, const QString &message);
    bool containsModel(const QString &modelId, ModelCategory category) const;
    QWebSocket *m_socket = nullptr;
    QUrl m_endpoint;
    QString m_token;
    bool m_authenticated = false;
    bool m_modelListAvailable = false;
    ServiceCapabilities m_capabilities;
    QList<ModelInfo> m_models;
    std::optional<ModelRouting> m_routing;
    QList<ModelInstall> m_installs;
    QHash<QString, PendingJob> m_pendingJobs;
    QQueue<QString> m_resultJobs;
    QString m_connectionErrorCode;
    QString m_connectionErrorDetail;
};
}
