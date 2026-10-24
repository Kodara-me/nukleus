#pragma once

#include <nukleus/ModelRouting.h>
#include <nukleus/ServiceProtocol.h>

#include "FfmpegRunner.h"
#include "LlamaServerRunner.h"
#include "SdCliRunner.h"

#include <QElapsedTimer>
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

// NUKLEUS_LLAMA_SERVER_IDLE_SECONDS overrides the ten-minute idle period before llama-server unloads.
inline int defaultLlamaServerIdleMs()
{
    bool ok = false;
    const int seconds = qEnvironmentVariableIntValue("NUKLEUS_LLAMA_SERVER_IDLE_SECONDS", &ok);
    return ok && seconds > 0 ? seconds * 1000 : 10 * 60 * 1000;
}

struct NukleusConfiguration
{
    QHostAddress listenAddress = QHostAddress::LocalHost;
    quint16 port = Nukleus::Protocol::DefaultPort;
    QString token;
    qint64 maximumImageBytes = 64LL * 1024 * 1024;
    QString modelRootPath;
    // With modelRootPath, selects the split layout: catalogue JSON files here, weights under modelRootPath/<category>/.
    QString catalogRootPath;
    QString llamaCompletion = qEnvironmentVariable("NUKLEUS_LLAMA_COMPLETION", "llama-completion");
    int promptEnhancementTimeoutMs = 10 * 60 * 1000; // Also bounds each text.chat turn, model loading included.
    QString llamaServer = qEnvironmentVariable("NUKLEUS_LLAMA_SERVER", "llama-server");
    int llamaServerIdleMs = defaultLlamaServerIdleMs();
    // Optional post-processing tool of video jobs (ping-pong loops); its features are offered only when it runs.
    QString ffmpeg = qEnvironmentVariable("NUKLEUS_FFMPEG", "ffmpeg");
};

class NukleusDaemon final : public QObject
{
    Q_OBJECT

public:
    explicit NukleusDaemon(NukleusConfiguration configuration, QObject *parent = nullptr);
    ~NukleusDaemon() override;
    bool start(QString *errorMessage = nullptr);
    quint16 serverPort() const;

private:
    enum class Operation { Generate, Upscale, Edit, PromptEnhance, TextChat, TextChatLoad, TextChatUnload, Animate };
    // An image.animate job's sd-cli run, resolved at submission; the uploads and output complete its arguments.
    struct AnimationPlan
    {
        QStringList arguments;
        QStringList files; // Model files the job reads, protected from deletion while it is pending.
        std::optional<int> totalSteps;
        bool twoPasses = false; // A high-noise pass runs before the low-noise one.
        QString container; // What the client asked for, and the returned file's.
        QString sdContainer; // What sd-cli writes: the same, except AVI for mp4, which ffmpeg encodes.
        int frames = 0;
        int fps = 0;
        std::optional<int> compressionQuality; // Also the quality of ffmpeg's encode.
        bool pingPong = false; // ffmpeg turns sd-cli's video into a forward-then-backward loop.
        // ffmpeg writes the returned file, so sd-cli's saving and completion are not the job's.
        bool postProcessed() const { return pingPong || container == QStringLiteral("mp4"); }
    };
    // A routed generation's submitted prompts, returned without preset prefixes.
    struct RoutedPrompts
    {
        QString prompt;
        QString negativePrompt;
    };
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
    bool startLlm(const QString &modelPath, const QJsonObject &parameters, const QString &system, const QString &input,
                  bool penalizeRepeats = true, const QStringList &extraArguments = {});
    void cancelJobsFor(QWebSocket *socket);
    void broadcastQueuePositions();
    void handleGenerationFinished(const SdCliGenerationResult &result);
    void handleUpscaleFinished(const SdCliUpscaleResult &result);
    bool isPromptEnhancementReady() const;
    // The MMPROJ companion's file for an LLM's assignment, or empty without a usable one.
    QString visionProjectorPath(const QString &mmprojModelId) const;
    void submitPromptEnhancement(QWebSocket* socket, const QJsonObject& message);
    void startPromptEnhancement();
    QStringList readyTextChatEngines() const;
    QStringList builtInChatTemplates(const QString &executable) const;
    void submitTextChat(QWebSocket* socket, const QJsonObject& message, Operation operation);
    void submitTextChatUnload(QWebSocket* socket, const QJsonObject& message);
    void submitAnimation(QWebSocket *socket, const QJsonObject &message);
    // Returns sd-cli's video, or starts its post-processing and returns false while it runs.
    bool finishAnimation(Job &job, const SdCliGenerationResult &result);
    void sendAnimationVideo(const Job &job, const QString &path, int frames);
    void handleFfmpegFinished(const FfmpegResult &result);
    void sendProgress(const Job &job, const Nukleus::GenerationProgress &progress) const;
    // sd-cli's --help, read once per executable; nullopt when it cannot be read.
    std::optional<SdCliHelp> sdCliHelp() const;
    // Checks the names of animation options listed by sd-cli's --help (samplers, schedulers, ...).
    bool validAnimationNames(const QJsonObject &parameters, QString *error) const;
    void startTextChat();
    void handleLlmFinished(int exitCode, QProcess::ExitStatus status);
    void handleLlamaServerFinished(const LlamaServerReply &reply);
    void sendChatResult(const Job &job, const QString &text, QJsonObject result);
    bool usesLlamaServer(const Job &job) const;
    void sendCapabilities(QWebSocket *socket) const;
    void send(QWebSocket *socket, QJsonObject object) const;
    void sendError(QWebSocket *socket, const QString &code, const QString &message, const QString &jobId = {}) const;
    bool isImageReady() const;
    bool modelInUse(const QString &path) const;
    std::shared_ptr<Job> jobForId(const QString &id) const;

    NukleusConfiguration m_configuration;
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
    std::unique_ptr<LlamaServerRunner> m_llamaServer;
    std::unique_ptr<FfmpegRunner> m_ffmpeg;
    QElapsedTimer m_activeJobClock;
    mutable QHash<QString, QStringList> m_chatTemplates; // Built-in chat template names per llama.cpp executable.
    mutable std::optional<SdCliHelp> m_sdCliHelp;
};
