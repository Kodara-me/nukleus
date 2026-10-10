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
namespace KiraService
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
signals:
    void serviceStateChanged(KiraService::ServiceConnectionState state, const QString &code, const QString &detail);
    void capabilitiesChanged(const KiraService::ServiceCapabilities &capabilities);
    void modelsChanged();
    void modelRoutingChanged();
    void modelRoutingConfigured();
    void modelRoutingFailed(const QString &message);
    void installsChanged();
    void modelConfigured(const QString &id, const QJsonObject &options);
    void modelStylesConfigured(const QString &id, const QList<KiraService::PromptStyle> &styles);
    void modelDeleted(const QString &id);
    void companionDeleted(const QString& id, const QString& role);
    void modelOperationFailed(const QString &id, const QString &message);
    void jobStateChanged(const QString &jobId, KiraService::JobState state, int queuePosition);
    void jobOutput(const QString &jobId, const QString &stream, const QString &text);
    void generationProgressChanged(const QString &jobId, const KiraService::GenerationProgress &progress);
    void imageJobFinished(const KiraService::ImageJobResult &result);
    void promptEnhancementFinished(const KiraService::PromptEnhancementResult& result);
    void chatFinished(const KiraService::TextChatResult& result);
    void chatModelFinished(const KiraService::ChatModelResult& result);
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
