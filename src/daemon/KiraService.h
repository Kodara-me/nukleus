#pragma once

#include <kira/service/ModelRouting.h>
#include <kira/service/ServiceProtocol.h>

#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QProcess>
#include <QTimer>
#include <QTemporaryDir>

#include <memory>
#include <vector>

class ModelCatalog;
class QWebSocket;
class QWebSocketServer;
class SdCliRunner;
struct SdCliGenerationResult;
struct SdCliUpscaleResult;

struct KiraServiceConfiguration
{
    QHostAddress listenAddress = QHostAddress::LocalHost;
    quint16 port = KiraService::Protocol::DefaultPort;
    QString token;
    qint64 maximumImageBytes = 64LL * 1024 * 1024;
    QString modelRootPath;
    QString llamaCompletion = qEnvironmentVariable("KIRA_LLAMA_COMPLETION", "llama-completion");
    int promptEnhancementTimeoutMs = 10 * 60 * 1000;
};

class KiraServiceDaemon final : public QObject
{
    Q_OBJECT

public:
    explicit KiraServiceDaemon(KiraServiceConfiguration configuration, QObject *parent = nullptr);
    ~KiraServiceDaemon() override;
    bool start(QString *errorMessage = nullptr);
    quint16 serverPort() const;

private:
    enum class Operation { Generate, Upscale, Edit, PromptEnhance, PromptFlow };
    struct PromptFlowRun;
    struct Submission;
    struct Job;

    void initializeBackend();
    void acceptConnection(QWebSocketServer *server);
    void disconnectClient(QWebSocket *socket);
    void processTextMessage(QWebSocket *socket, const QString &message);
    void processBinaryMessage(QWebSocket *socket, const QByteArray &data);
    void submit(QWebSocket *socket, const QJsonObject &message);
    void listModels(QWebSocket *socket) const;
    void installModel(QWebSocket *socket, const QJsonObject &message);
    void configureModel(QWebSocket *socket, const QJsonObject &message);
    void configureModelStyles(QWebSocket *socket, const QJsonObject &message);
    void modelRouting(QWebSocket *socket, const QJsonObject &message);
    void cancelInstall(QWebSocket *socket, const QJsonObject &message);
    void deleteModel(QWebSocket *socket, const QJsonObject &message);
    void deleteCompanion(QWebSocket* socket, const QJsonObject& message);
    void enqueue(std::shared_ptr<Job> job);
    void startNextJob();
    void runActiveJob();
    void startPromptFlowStep();
    bool startLlm(const QString &modelPath, const QJsonObject &parameters, const QString &system, const QString &input,
                  bool penalizeRepeats = true);
    void cancelJobsFor(QWebSocket *socket);
    void broadcastQueuePositions();
    void handleGenerationFinished(const SdCliGenerationResult &result);
    void handleUpscaleFinished(const SdCliUpscaleResult &result);
    bool isPromptEnhancementReady() const;
    void submitPromptEnhancement(QWebSocket* socket, const QJsonObject& message);
    void submitPromptFlow(QWebSocket *socket, const QJsonObject &message);
    bool preparePromptFlow(QWebSocket *socket, const QString &jobId, const KiraService::GenerationPreset &preset,
                           PromptFlowRun &flow) const;
    void startPromptEnhancement();
    void handlePromptEnhancementFinished(int exitCode, QProcess::ExitStatus status);
    void sendCapabilities(QWebSocket *socket) const;
    void send(QWebSocket *socket, QJsonObject object) const;
    void sendError(QWebSocket *socket, const QString &code, const QString &message, const QString &jobId = {}) const;
    bool isImageReady() const;
    bool modelInUse(const QString &path) const;
    std::shared_ptr<Job> jobForId(const QString &id) const;

    KiraServiceConfiguration m_configuration;
    std::vector<std::unique_ptr<QWebSocketServer>> m_servers;
    std::unique_ptr<ModelCatalog> m_models;
    std::unique_ptr<SdCliRunner> m_sdRunner;
    QHash<QWebSocket *, bool> m_authenticatedClients;
    QHash<QWebSocket *, std::shared_ptr<Submission>> m_pendingUploads;
    QHash<QString, QPointer<QWebSocket>> m_installClients;
    QQueue<std::shared_ptr<Job>> m_queue;
    std::shared_ptr<Job> m_activeJob;
    QProcess m_promptProcess;
    QTimer m_promptTimeout;
    QByteArray m_promptOutput;
    QByteArray m_promptError;
    bool m_promptTimedOut = false;
    bool m_promptOutputExceeded = false;
};
