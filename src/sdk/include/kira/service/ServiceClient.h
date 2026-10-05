#pragma once
#include "GenerationProgress.h"
#include "ImageTypes.h"
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
    QList<ModelInstall> installs() const;
    void requestModels();
    QString installModel(ModelCategory category, const QUrl &url, const QUrl &companionUrl = {}, const QJsonObject &recommendedOptions = {}, const QJsonObject &companionUrls = {},
        const QString &modelType = QStringLiteral("checkpoint"), const QString &existingModelId = {}, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {});
    bool configureModel(const QString &id, const QJsonObject &options, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {});
    bool configureModelStyles(const QString &id, const QList<PromptStyle> &styles);
    bool cancelInstall(const QString &installId);
    bool deleteModel(const QString &id);
    bool deleteCompanion(const QString& id, const QString& role);
    QString submitEdit(const ImageGenerationRequest &request);
    QString submitGeneration(const ImageGenerationRequest &request);
    QString submitUpscale(const ImageUpscaleRequest &request);
    QString submitFaceParsing(const ImageUpscaleRequest &request);
    QString submitInpaint(const ImageGenerationRequest& request, const QByteArray& maskImageData);
    QString exportPhotoMakerIdentity(const QByteArray &sourceImageData);
    QString exportPhotoMakerIdentityImages(const QList<QByteArray>& sourceImages);
    bool canEnhancePrompt(const QString& modelId) const;
    QString enhancePrompt(const PromptEnhancementRequest& request);
signals:
    void serviceStateChanged(KiraService::ServiceConnectionState state, const QString &code, const QString &detail);
    void capabilitiesChanged(const KiraService::ServiceCapabilities &capabilities);
    void modelsChanged();
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
    void photoMakerIdentityExportFinished(const KiraService::PhotoMakerIdentityResult &result);
    void promptEnhancementFinished(const KiraService::PromptEnhancementResult& result);
private:
    struct PendingJob
    {
        ImageOperation operation = ImageOperation::Generate;
        QByteArray inputData;
        QByteArray ipAdapterImageData;
        QByteArray maskImageData;
        QString mimeType;
        bool identityExport = false;
        QByteArray photoMakerIdentityData;
        bool promptEnhancement = false;
        QList<QByteArray> referenceImages;
    };
    QString submit(ImageOperation operation, const QJsonObject &options, const QByteArray &inputData, const QByteArray &ipAdapterImageData = {}, const QByteArray &photoMakerIdentityData = {}, const QList<QByteArray>& references = {}, const QByteArray& maskImageData = {});
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
    QList<ModelInstall> m_installs;
    QHash<QString, PendingJob> m_pendingJobs;
    QQueue<QString> m_resultJobs;
    QString m_connectionErrorCode;
    QString m_connectionErrorDetail;
};
}
