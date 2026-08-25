#pragma once
#include "GenerationProgress.h"
#include "ImageTypes.h"
#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QSet>
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
    bool canConvertPrompt(const QString &modelId) const;
    bool modelListAvailable() const;
    QList<ModelInfo> models(ModelCategory category = ModelCategory::Unknown) const;
    QList<ModelInstall> installs() const;
    void requestModels();
    QString installModel(ModelCategory category, const QUrl &url, const QString &sha256 = {});
    bool deleteModel(const QString &id);
    QString submitGeneration(const ImageGenerationRequest &request);
    QString submitUpscale(const ImageUpscaleRequest &request);
    QString submitPromptConversion(const PromptConversionRequest &request);
signals:
    void serviceStateChanged(KiraService::ServiceConnectionState state, const QString &code, const QString &detail);
    void capabilitiesChanged(const KiraService::ServiceCapabilities &capabilities);
    void modelsChanged();
    void installsChanged();
    void modelDeleted(const QString &id);
    void modelOperationFailed(const QString &id, const QString &message);
    void jobStateChanged(const QString &jobId, KiraService::JobState state, int queuePosition);
    void jobOutput(const QString &jobId, const QString &stream, const QString &text);
    void generationProgressChanged(const QString &jobId, const KiraService::GenerationProgress &progress);
    void imageJobFinished(const KiraService::ImageJobResult &result);
    void promptConversionFinished(const KiraService::PromptConversionResult &result);
private:
    struct PendingJob
    {
        ImageOperation operation = ImageOperation::Generate;
        QByteArray inputData;
        QString mimeType;
    };
    QString submit(ImageOperation operation, const QJsonObject &options, const QByteArray &inputData);
    void authenticate();
    void handleTextMessage(const QString &message);
    void handleBinaryMessage(const QByteArray &data);
    void updateCapabilities(const ServiceCapabilities &capabilities);
    void clearCatalog();
    ModelInstall *findInstall(const QString &installId);
    void failActiveInstalls(const QString &message);
    void failJob(const QString &jobId, const QString &errorCode, const QString &message);
    void failAllJobs(const QString &errorCode, const QString &message);
    void failPromptConversion(const QString &jobId, const QString &errorCode, const QString &message);
    void failAllPromptConversions(const QString &errorCode, const QString &message);
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
    QSet<QString> m_pendingPromptConversions;
    QQueue<QString> m_resultJobs;
    QString m_connectionErrorCode;
    QString m_connectionErrorDetail;
};
}
