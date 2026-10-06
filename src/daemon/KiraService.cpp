#include "KiraService.h"

#include "GenerationRequest.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include <kira/service/ServiceProtocol.h>
#include "SdCliRunner.h"

#include <QDebug>
#include <QDir>
#include <QtEndian>
#include <cmath>
#include <algorithm>
#include <limits>
#include <kira/service/ModelOptions.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

namespace
{
constexpr int MaximumQueuedJobs = 8;

bool validNumber(const QJsonObject &object, const QString &name, double minimum, double maximum,
                 bool integer, bool required = true)
{
    if (!object.contains(name)) return !required;
    const auto value = object.value(name);
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && number >= minimum && number <= maximum
        && (!integer || std::floor(number) == number);
}

bool validGenerationOptions(const QJsonObject &options)
{
    const auto optionalString = [&options](const QString &key) {
        return !options.contains(key) || options.value(key).isString();
    };
    const auto validChoice = [&options](const QString &key, const QStringList &choices) {
        return !options.contains(key) || (options.value(key).isString() && choices.contains(options.value(key).toString()));
    };
    return options.value("prompt").isString() && !options.value("prompt").toString().trimmed().isEmpty()
        && optionalString("negativePrompt")
        && validNumber(options, "width", 64, 4096, true, false)
        && validNumber(options, "height", 64, 4096, true, false)
        && validNumber(options, "steps", 1, 200, true, false)
        && validNumber(options, "cfgScale", 0, 100, false, false)
        && validNumber(options, "seed", -1, std::numeric_limits<int>::max(), true, false)
        && validNumber(options, "baseImageStrength", 0, 1, false, false)
        && (!options.contains("clipSkip") || (validNumber(options, "clipSkip", -1, 12, true)
                                             && options.value("clipSkip").toInt() != 0))
        && validChoice("sampler", KiraService::Protocol::modelSamplers())
        && validChoice("scheduler", KiraService::Protocol::modelSchedulers())
        && (!options.contains("hasInput") || options.value("hasInput").isBool());
}

}

struct KiraServiceDaemon::Submission
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    QString modelPath;
    QString vaePath;
    QString llmPath;
    QString llmVisionPath;
    QString clipLPath;
    QString clipGPath;
    bool diffusionModel = false;
    QString clipVisionPath;
    QString ipAdapterPath;
    QString ipAdapterLoraPath;
    QPointer<QWebSocket> client;
    bool waitingIpAdapterImage = false;
    QString photoMakerPath;
    bool waitingPhotoMakerIdentity = false;
    bool waitingPhotoMakerReferences = false;
    QByteArray photoMakerIdentityData;
    QByteArray inputData;
    QList<QByteArray> referenceImages;
    int referenceCount = 1;
    QByteArray ipAdapterImageData;
    QByteArray maskImageData;
    bool waitingMask = false;
};

struct KiraServiceDaemon::Job
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    QString modelPath;
    QString vaePath;
    QString llmPath;
    QString llmVisionPath;
    QString clipLPath;
    QString clipGPath;
    bool diffusionModel = false;
    QString clipVisionPath;
    QString ipAdapterPath;
    QString ipAdapterLoraPath;
    QPointer<QWebSocket> client;
    std::unique_ptr<QTemporaryDir> directory = std::make_unique<QTemporaryDir>();
    QString inputPath;
    QString maskPath;
    QString ipAdapterImagePath;
    QString outputPath;
    QString photoMakerPath;
    QString photoMakerIdentityPath;
    QString photoMakerImagesDirectory;
};

KiraServiceDaemon::KiraServiceDaemon(KiraServiceConfiguration configuration, QObject *parent)
    : QObject(parent)
    , m_configuration(std::move(configuration))
    , m_models(m_configuration.modelRootPath.isEmpty()
                   ? std::make_unique<ModelCatalog>()
                   : std::make_unique<ModelCatalog>(m_configuration.modelRootPath, false))
{
    m_promptTimeout.setSingleShot(true);
    m_promptTimeout.setInterval(m_configuration.promptEnhancementTimeoutMs);
    connect(&m_promptTimeout, &QTimer::timeout, this, [this] {
        m_promptTimedOut = true;
        m_promptProcess.kill();
    });
    connect(&m_promptProcess, &QProcess::readyReadStandardError, this, [this] {
        m_promptError = (m_promptError + m_promptProcess.readAllStandardError()).right(4096);
    });
    connect(&m_promptProcess, &QProcess::readyReadStandardOutput, this, [this] {
        const auto bytes = m_promptProcess.readAllStandardOutput();
        if (m_promptOutput.size() + bytes.size() > 1024 * 1024) {
            m_promptOutputExceeded = true;
            m_promptProcess.kill();
        } else m_promptOutput += bytes;
    });
    connect(&m_promptProcess, &QProcess::started, this, [this] { m_promptProcess.closeWriteChannel(); });
    connect(&m_promptProcess, &QProcess::finished, this, &KiraServiceDaemon::handlePromptEnhancementFinished);
    connect(&m_promptProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_promptError = m_promptProcess.errorString().toUtf8();
            handlePromptEnhancementFinished(-1, QProcess::CrashExit);
        }
    });
    m_photoMakerTimeout.setSingleShot(true);
    m_faceParsingTimeout.setSingleShot(true);
    m_faceParsingTimeout.setInterval(m_configuration.faceParsingTimeoutMs);
    connect(&m_faceParsingTimeout, &QTimer::timeout, this, [this] {
        m_faceParsingTimedOut = true;
        m_faceParsingProcess.kill();
    });
    connect(&m_faceParsingProcess, &QProcess::readyReadStandardError, this, [this] {
        m_faceParsingError = (m_faceParsingError + m_faceParsingProcess.readAllStandardError()).right(4096);
    });
    connect(&m_faceParsingProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_faceParsingProcess.readAllStandardOutput();
    });
    connect(&m_faceParsingProcess, &QProcess::finished, this, &KiraServiceDaemon::handleFaceParsingFinished);
    connect(&m_faceParsingProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_faceParsingError = m_faceParsingProcess.errorString().toUtf8();
            handleFaceParsingFinished(-1, QProcess::CrashExit);
        }
    });
    m_photoMakerTimeout.setInterval(10 * 60 * 1000);
    connect(&m_photoMakerTimeout, &QTimer::timeout, this, [this] {
        m_photoMakerTimedOut = true;
        m_photoMakerProcess.kill();
    });
    connect(&m_photoMakerProcess, &QProcess::readyReadStandardError, this, [this] {
        m_photoMakerError = (m_photoMakerError + m_photoMakerProcess.readAllStandardError()).right(4096);
    });
    connect(&m_photoMakerProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_photoMakerProcess.readAllStandardOutput();
    });
    connect(&m_photoMakerProcess, &QProcess::finished, this, &KiraServiceDaemon::handlePhotoMakerExportFinished);
    connect(&m_photoMakerProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_photoMakerError = m_photoMakerProcess.errorString().toUtf8();
            handlePhotoMakerExportFinished(-1, QProcess::CrashExit);
        }
    });
    connect(m_models.get(), &ModelCatalog::installProgress, this, [this](const ModelInstallProgress &progress) {
        const QPointer<QWebSocket> client = m_installClients.value(progress.installId);
        QJsonObject event{{QStringLiteral("type"), QStringLiteral("models.install.progress")},
                          {QStringLiteral("installId"), progress.installId},
                          {QStringLiteral("state"), progress.state == ModelInstallState::Downloading ? QStringLiteral("downloading") : QStringLiteral("verifying")},
                          {QStringLiteral("bytesDownloaded"), progress.bytesDownloaded}, {"fileRole", progress.fileRole}};
        if (progress.totalBytes.has_value()) event.insert(QStringLiteral("totalBytes"), *progress.totalBytes);
        send(client, event);
    });
    connect(m_models.get(), &ModelCatalog::installFinished, this, [this](const QString &installId, const ModelInfo &model) {
        const QPointer<QWebSocket> client = m_installClients.take(installId);
        QJsonObject modelObject{{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.name},
                                {QStringLiteral("fileName"), model.fileName}, {QStringLiteral("bytes"), model.fileSize}};
        modelObject.insert("explicitContent", model.explicitContent);
        modelObject.insert("canEdit", model.canEdit);
        modelObject.insert("displayName", model.displayName);
        if (!model.recommendedOptions.isEmpty()) modelObject.insert("recommendedOptions", model.recommendedOptions);
        modelObject.insert("companionFiles", model.companionFiles);
        modelObject.insert("modelType", model.modelType);
        if (!model.companionFileName.isEmpty()) {
            modelObject.insert(QStringLiteral("companionFileName"), model.companionFileName);
            modelObject.insert(QStringLiteral("companionRole"), model.companionRole);
        }
        send(client, {{QStringLiteral("type"), QStringLiteral("models.install.completed")},
                      {QStringLiteral("installId"), installId}, {QStringLiteral("model"), modelObject}});
        if (model.canEdit) {
            for (auto it = m_authenticatedClients.cbegin(); it != m_authenticatedClients.cend(); ++it)
                if (it.value()) send(it.key(), {{"type", "models.changed"}, {"id", model.id}});
        }
    });
    connect(m_models.get(), &ModelCatalog::installCanceled, this, [this](const QString &id) {
        const auto client = m_installClients.take(id);
        send(client, {{"type", "models.install.canceled"}, {"installId", id}});
    });
    connect(m_models.get(), &ModelCatalog::installFailed, this, [this](const QString &installId, const QString &errorMessage) {
        const QPointer<QWebSocket> client = m_installClients.take(installId);
        if (client) send(client, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("model-install-failed")},
                                  {QStringLiteral("message"), errorMessage}, {QStringLiteral("installId"), installId}});
    });
}

KiraServiceDaemon::~KiraServiceDaemon()
{
    // Closing the server while the client bookkeeping is still alive prevents
    // disconnect callbacks from accessing members already being destroyed.
    m_servers.clear();
    m_faceParsingProcess.disconnect(this);
    m_faceParsingProcess.kill();
    m_faceParsingProcess.waitForFinished(3000);
    m_promptProcess.disconnect(this);
    m_promptProcess.kill();
    m_promptProcess.waitForFinished(3000);
    m_photoMakerProcess.disconnect(this);
    m_photoMakerProcess.kill();
    m_photoMakerProcess.waitForFinished(3000);
}

bool KiraServiceDaemon::start(QString *errorMessage)
{
    if (!m_configuration.listenAddress.isLoopback()
        && (m_configuration.listenAddress.protocol() != QAbstractSocket::IPv4Protocol
            || m_configuration.listenAddress == QHostAddress::AnyIPv4
            || m_configuration.listenAddress == QHostAddress::Any)) {
        if (errorMessage) *errorMessage = tr("The LAN listen address must be an IPv4 address");
        return false;
    }
    if (!m_configuration.listenAddress.isLoopback() && m_configuration.token.isEmpty()) {
        if (errorMessage) *errorMessage = tr("A token is required when listening on the LAN");
        return false;
    }
    if (m_configuration.modelRootPath.isEmpty() && !ServicePaths::ensureModelDirectories(errorMessage)) return false;
    m_servers.clear();
    QList<QHostAddress> addresses{QHostAddress(QStringLiteral("127.0.0.1"))};
    if (!m_configuration.listenAddress.isLoopback()) addresses.append(m_configuration.listenAddress);
    quint16 port = m_configuration.port;
    for (const QHostAddress &address : addresses) {
        auto server = std::make_unique<QWebSocketServer>(QStringLiteral("Kira Service"), QWebSocketServer::NonSecureMode, this);
        if (!server->listen(address, port)) {
            if (errorMessage) *errorMessage = tr("Could not listen on %1:%2: %3").arg(address.toString()).arg(port).arg(server->errorString());
            m_servers.clear();
            return false;
        }
        if (port == 0) port = server->serverPort();
        QWebSocketServer *serverPointer = server.get();
        connect(serverPointer, &QWebSocketServer::newConnection, this, [this, serverPointer] { acceptConnection(serverPointer); });
        m_servers.push_back(std::move(server));
    }
    initializeBackend();
    return true;
}

quint16 KiraServiceDaemon::serverPort() const
{
    return m_servers.empty() ? 0 : m_servers.front()->serverPort();
}

void KiraServiceDaemon::initializeBackend()
{
    const QString sdCliExecutable = QStandardPaths::findExecutable(QStringLiteral("sd-cli"));
    if (sdCliExecutable.isEmpty()) {
        qCritical().noquote() << QStringLiteral("[Kira Service] sd-cli was not found on the service PATH.");
    } else {
        m_sdRunner = std::make_unique<SdCliRunner>(sdCliExecutable);
        connect(m_sdRunner.get(), &SdCliRunner::generationProgressChanged, this, [this](const KiraService::GenerationProgress &progress) {
            if (m_activeJob) send(m_activeJob->client, {{QStringLiteral("type"), QStringLiteral("progress")}, {QStringLiteral("jobId"), m_activeJob->id}, {QStringLiteral("progress"), KiraService::Protocol::progressToJson(progress)}});
        });
        connect(m_sdRunner.get(), &SdCliRunner::generationOutput, this, [this](const QString &stream, const QString &text) {
            if (m_activeJob) send(m_activeJob->client, {{QStringLiteral("type"), QStringLiteral("output")}, {QStringLiteral("jobId"), m_activeJob->id}, {QStringLiteral("stream"), stream}, {QStringLiteral("text"), text}});
        });
        connect(m_sdRunner.get(), &SdCliRunner::generationFinished, this, &KiraServiceDaemon::handleGenerationFinished);
        connect(m_sdRunner.get(), &SdCliRunner::upscaleFinished, this, &KiraServiceDaemon::handleUpscaleFinished);
    }
}

bool KiraServiceDaemon::isImageReady() const
{
    return m_sdRunner != nullptr;
}

void KiraServiceDaemon::acceptConnection(QWebSocketServer *server)
{
    QWebSocket *socket = server->nextPendingConnection();
    socket->setMaxAllowedIncomingMessageSize(m_configuration.maximumImageBytes + 4096);
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &message) { processTextMessage(socket, message); });
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray &data) { processBinaryMessage(socket, data); });
    connect(socket, &QWebSocket::disconnected, this, [this, socket] { disconnectClient(socket); });
}

void KiraServiceDaemon::disconnectClient(QWebSocket *socket)
{
    cancelJobsFor(socket);
    QStringList installIds;
    for (auto it = m_installClients.cbegin(); it != m_installClients.cend(); ++it)
        if (it.value() == socket) installIds.append(it.key());
    for (const QString &installId : installIds) m_models->cancel(installId);
    m_authenticatedClients.remove(socket);
    m_pendingUploads.remove(socket);
    socket->deleteLater();
}

void KiraServiceDaemon::processTextMessage(QWebSocket *socket, const QString &message)
{
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) { sendError(socket, QStringLiteral("invalid-message"), tr("Expected a JSON object")); return; }
    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (!m_authenticatedClients.contains(socket)) {
        const bool loopbackPeer = socket->peerAddress().isLoopback();
        const QString token = object.value(QStringLiteral("token")).toString();
        if (type != QStringLiteral("authenticate") || object.value(QStringLiteral("version")).toInt() != KiraService::Protocol::Version
            || (!loopbackPeer && (m_configuration.token.isEmpty() || token != m_configuration.token))) {
            qWarning().noquote() << QStringLiteral("[Kira Service] Rejected authentication from %1.").arg(socket->peerAddress().toString());
            sendError(socket, QStringLiteral("authentication-failed"), tr("Authentication failed")); socket->close(); return;
        }
        m_authenticatedClients.insert(socket, true);
        qInfo().noquote() << QStringLiteral("[Kira Service] Authenticated client from %1.").arg(socket->peerAddress().toString());
        send(socket, {{QStringLiteral("type"), QStringLiteral("authenticated")}});
        sendCapabilities(socket);
        return;
    }
    if (type == QStringLiteral("submit")) submit(socket, object);
    else if (type == QStringLiteral("capabilities")) sendCapabilities(socket);
    else if (type == QStringLiteral("models.list")) listModels(socket);
    else if (type == QStringLiteral("models.install")) installModel(socket, object);
    else if (type == QStringLiteral("models.configure")) configureModel(socket, object);
    else if (type == QStringLiteral("models.styles.configure")) configureModelStyles(socket, object);
    else if (type == QStringLiteral("models.install.cancel")) cancelInstall(socket, object);
    else if (type == QStringLiteral("models.companion.delete")) deleteCompanion(socket, object);
    else if (type == QStringLiteral("models.delete")) deleteModel(socket, object);
    else sendError(socket, QStringLiteral("unsupported-message"), tr("Unsupported message"));
}

void KiraServiceDaemon::submit(QWebSocket *socket, const QJsonObject &message)
{
    const QString operationName = message.value(QStringLiteral("operation")).toString();
    const bool generate = operationName == QStringLiteral("image.generate");
    const bool edit = operationName == QStringLiteral("image.edit");
    const bool upscale = operationName == QStringLiteral("image.upscale");
    const bool identityExport = operationName == QStringLiteral("photomaker.export");
    const bool promptEnhance = operationName == QStringLiteral("prompt.enhance");
    const bool faceParsing = operationName == QStringLiteral("image.face-parsing");
    const bool inpaint = operationName == QStringLiteral("image.inpaint");
    QJsonObject options = message.value(QStringLiteral("options")).toObject();
    const QString jobId = message.value(QStringLiteral("jobId")).toString();
    bool duplicateId = jobForId(jobId) != nullptr;
    for (const auto &pending : m_pendingUploads) {
        duplicateId = duplicateId || pending->id == jobId;
    }
    if ((!generate && !upscale && !edit && !identityExport && !promptEnhance && !faceParsing && !inpaint) || jobId.isEmpty() || duplicateId) { sendError(socket, QStringLiteral("invalid-request"), tr("Invalid job request"), jobId); return; }
    if (promptEnhance) { submitPromptEnhancement(socket, message); return; }
    if (faceParsing) {
        if (!isFaceParsingReady()) {
            sendError(socket, "face-parsing-unavailable", tr("Face parsing Python is not configured"), jobId); return;
        }
        if (!message.value("options").isObject() || options.keys() != QStringList{"modelId"}
            || !options.value("modelId").isString()) {
            sendError(socket, "invalid-request", tr("Face parsing accepts only a model ID and source image"), jobId); return;
        }
        const auto model = m_models->model(ModelCategory::FaceParsing, options.value("modelId").toString());
        if (!model) { sendError(socket, "model-unavailable", tr("The requested face parsing model is unavailable"), jobId); return; }
        if (m_pendingUploads.contains(socket) || m_queue.size() >= MaximumQueuedJobs) {
            sendError(socket, "queue-full", tr("An upload is pending or the service queue is full"), jobId); return;
        }
        const auto recommendations = model->recommendedOptions.isEmpty()
            ? KiraService::Protocol::defaultModelOptions("face-parsing") : model->recommendedOptions;
        if (!KiraService::Protocol::validateModelOptions("face-parsing", recommendations)) {
            sendError(socket, "invalid-request", tr("Invalid face parsing model recommendations"), jobId); return;
        }
        auto submission = std::make_shared<Submission>();
        submission->id = jobId;
        submission->operation = Operation::FaceParsing;
        submission->options = recommendations;
        submission->modelPath = model->filePath;
        submission->client = socket;
        m_pendingUploads.insert(socket, submission);
        send(socket, {{"type", "upload-required"}, {"jobId", jobId}, {"inputRole", "source"}});
        return;
    }
    if (identityExport) {
        if (!isPhotoMakerExportReady()) {
            sendError(socket, "photomaker-unavailable", tr("PhotoMaker identity extraction is not configured"), jobId);
            return;
        }
        if (m_pendingUploads.contains(socket) || m_queue.size() >= MaximumQueuedJobs) {
            sendError(socket, "queue-full", tr("An upload is pending or the service queue is full"), jobId);
            return;
        }
        if (!message.value("options").isObject() || options.size() > (options.contains("referenceCount") ? 1 : 0)
            || !validNumber(options, "referenceCount", 1, 4, true, false)) {
            sendError(socket, "invalid-request", tr("Identity export accepts only a source image"), jobId);
            return;
        }
        auto submission = std::make_shared<Submission>();
        submission->id = jobId;
        submission->operation = Operation::PhotoMakerExport;
        submission->referenceCount = options.value("referenceCount").toInt(1);
        submission->client = socket;
        m_pendingUploads.insert(socket, submission);
        send(socket, {{"type", "upload-required"}, {"jobId", jobId}, {"inputRole", "source"}});
        return;
    }
    if (!isImageReady()) {
        sendError(socket, QStringLiteral("backend-not-ready"), tr("The requested service backend is unavailable"), jobId);
        return;
    }
    const ModelCategory category = inpaint ? ModelCategory::Inpaint : edit ? ModelCategory::Edition : generate ? ModelCategory::ImageGeneration : ModelCategory::Upscaler;
    const std::optional<ModelInfo> model = m_models->model(category, options.value(QStringLiteral("modelId")).toString());
    if (!model.has_value()) { sendError(socket, QStringLiteral("model-unavailable"), tr("The requested model is unavailable"), jobId); return; }
    if (upscale && options.contains(QStringLiteral("scale"))) {
        sendError(socket, QStringLiteral("invalid-request"), tr("Upscale is fixed at 2×"), jobId);
        return;
    }
    if (m_queue.size() >= MaximumQueuedJobs) { sendError(socket, QStringLiteral("queue-full"), tr("The service queue is full"), jobId); return; }
    if ((generate || edit || inpaint) && (!validGenerationOptions(options)
        || (edit && options.value("prompt").toString().trimmed().isEmpty()))) {
        sendError(socket, QStringLiteral("invalid-request"), tr("Invalid generation options"), jobId);
        return;
    }
    if ((inpaint && (!options.value("hasMask").isBool() || !options.value("hasMask").toBool()
            || !options.value("hasInput").isBool() || !options.value("hasInput").toBool()
            || !validNumber(options, "width", 64, 4096, true)
            || !validNumber(options, "height", 64, 4096, true)))
        || (!inpaint && options.contains("hasMask"))) {
        sendError(socket, "invalid-request", tr("Inpainting requires a source image and mask"), jobId); return;
    }
    if (options.contains("styleId") && (!options.value("styleId").isString()
        || (!generate && !options.value("styleId").toString().isEmpty()))) {
        sendError(socket, "invalid-request", tr("Styles are only supported for image generation"), jobId);
        return;
    }
    const QString styleId = options.value("styleId").toString();
    if (!styleId.isEmpty()) {
        const auto style = std::find_if(model->styles.cbegin(), model->styles.cend(),
            [&styleId](const KiraService::PromptStyle &candidate) { return candidate.id == styleId; });
        if (style == model->styles.cend()) {
            sendError(socket, "style-unavailable", tr("The requested style is unavailable for this model"), jobId);
            return;
        }
        options.insert("prompt", KiraService::Protocol::composeStyledPrompt(style->positivePrefix, options.value("prompt").toString()));
        options.insert("negativePrompt", KiraService::Protocol::composeStyledPrompt(style->negativePrefix, options.value("negativePrompt").toString()));
    }
    const bool hasAdapterOptions = options.contains("clipVisionModelId") || options.contains("ipAdapterModelId")
        || options.contains("hasIpAdapterImage") || options.contains("ip_adapter_strength");
    std::optional<ModelInfo> clipVision;
    std::optional<ModelInfo> ipAdapter;
    const bool hasPhotoMaker = options.contains("photoMakerModelId")
        || options.contains("hasPhotoMakerIdentity") || options.contains("photoMakerStyleStrength");
    if (options.contains("referenceCount") && !hasPhotoMaker) {
        sendError(socket, "invalid-request", tr("Reference count requires PhotoMaker conditioning"), jobId); return;
    }
    std::optional<ModelInfo> photoMaker;
    if (hasPhotoMaker) {
        if (!validNumber(options, "referenceCount", 1, 4, true, false)) {
            sendError(socket, "invalid-request", tr("Invalid PhotoMaker reference count"), jobId); return;
        }
        if ((!generate && !inpaint) || hasAdapterOptions || (generate && options.value("hasInput").toBool())
            || !options.value("photoMakerModelId").isString()
            || !options.value("hasPhotoMakerIdentity").isBool() || !options.value("hasPhotoMakerIdentity").toBool()
            || !validNumber(options, "photoMakerStyleStrength", 0, 100, true, false)) {
            sendError(socket, "invalid-request", tr("PhotoMaker generation requires a model, identity embeddings and a reference image"), jobId);
            return;
        }
        photoMaker = m_models->model(ModelCategory::PhotoMaker, options.value("photoMakerModelId").toString());
        if (!photoMaker) {
            sendError(socket, "model-unavailable", tr("The requested PhotoMaker model is unavailable"), jobId);
            return;
        }
    }
    if (hasAdapterOptions) {
        if (upscale || !options.value("clipVisionModelId").isString()
            || !options.value("ipAdapterModelId").isString()
            || !options.value("hasIpAdapterImage").isBool() || !options.value("hasIpAdapterImage").toBool()
            || !validNumber(options, "ip_adapter_strength", 0, 2, false, false)) {
            sendError(socket, "invalid-request", tr("IP-Adapter requires both model IDs and a reference image"), jobId);
            return;
        }
        clipVision = m_models->model(ModelCategory::ClipVision, options.value("clipVisionModelId").toString());
        ipAdapter = m_models->model(ModelCategory::IpAdapter, options.value("ipAdapterModelId").toString());
        if (!clipVision || !ipAdapter) {
            sendError(socket, "model-unavailable", tr("The requested IP-Adapter or CLIP Vision model is unavailable"), jobId);
            return;
        }
    }
    if (m_pendingUploads.contains(socket)) {
        sendError(socket, "upload-in-progress", tr("Finish the pending image upload before submitting another job"), jobId);
        return;
    }
    const bool needsInput = inpaint || edit || upscale || options.value(QStringLiteral("hasInput")).toBool();
    auto submission = std::make_shared<Submission>();
    submission->id = jobId;
    submission->operation = inpaint ? Operation::Inpaint : edit ? Operation::Edit : generate ? Operation::Generate : Operation::Upscale;
    submission->options = options;
    submission->modelPath = model->filePath;
    if (photoMaker) {
        submission->photoMakerPath = photoMaker->filePath;
        submission->referenceCount = options.value("referenceCount").toInt(1);
    }
    if ((generate || edit || inpaint) && model->companionRole == QStringLiteral("vae") && !model->companionFileName.isEmpty())
        submission->vaePath = QDir(QFileInfo(model->filePath).absolutePath()).filePath(model->companionFileName);
    const QDir modelDirectory(QFileInfo(model->filePath).absolutePath());
    const auto filePath = [&](const QString &role) {
        const auto name = model->companionFiles.value(role).toString();
        return name.isEmpty() ? QString() : modelDirectory.filePath(name);
    };
    if (generate || edit || inpaint) {
        if (model->companionFiles.contains("vae")) submission->vaePath = filePath("vae");
        submission->llmPath = filePath("llm");
        submission->llmVisionPath = filePath("mmproj");
        submission->clipLPath = filePath("clip-l");
        submission->clipGPath = filePath("clip-g");
        submission->diffusionModel = model->modelType == "diffusion";
    }
    if (hasAdapterOptions) {
        submission->clipVisionPath = clipVision->filePath;
        submission->ipAdapterPath = ipAdapter->filePath;
        const auto lora = ipAdapter->companionFiles.value("lora").toString();
        if (!lora.isEmpty()) {
            submission->ipAdapterLoraPath = QDir(QFileInfo(ipAdapter->filePath).absolutePath()).filePath(lora);
            const auto name = QFileInfo(submission->ipAdapterLoraPath).fileName();
            if (name.contains(':') || name.contains('<') || name.contains('>')
                || name.startsWith("|high_noise|") || options.value("prompt").toString().contains("<lora:")) {
                sendError(socket, "invalid-request", tr("The auxiliary LoRA filename or prompt contains an unsupported LoRA tag"), jobId);
                return;
            }
        }
    }
    submission->client = socket;
    submission->waitingIpAdapterImage = !needsInput && hasAdapterOptions;
    if (needsInput || hasAdapterOptions || hasPhotoMaker) {
        m_pendingUploads.insert(socket, submission);
        send(socket, {{QStringLiteral("type"), QStringLiteral("upload-required")}, {QStringLiteral("jobId"), jobId},
            {"inputRole", hasPhotoMaker && !inpaint ? "photomaker-reference" : submission->waitingIpAdapterImage ? "ip-adapter" : "source"}});
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = jobId;
    job->operation = submission->operation;
    job->options = options;
    job->modelPath = model->filePath;
    job->vaePath = submission->vaePath;
    job->llmPath = submission->llmPath;
    job->llmVisionPath = submission->llmVisionPath;
    job->clipLPath = submission->clipLPath;
    job->clipGPath = submission->clipGPath;
    job->diffusionModel = submission->diffusionModel;
    job->clipVisionPath = submission->clipVisionPath;
    job->ipAdapterPath = submission->ipAdapterPath;
    job->ipAdapterLoraPath = submission->ipAdapterLoraPath;
    job->client = socket;
    enqueue(job);
}

void KiraServiceDaemon::listModels(QWebSocket *socket) const
{
    QJsonArray categories;
    for (const ModelCategory category : ModelCatalog::categories()) {
        QJsonArray models;
        for (const ModelInfo &model : m_models->models(category)) {
            QJsonObject modelObject{{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.name},
                                    {QStringLiteral("fileName"), model.fileName}, {QStringLiteral("bytes"), model.fileSize}};
            modelObject.insert("explicitContent", model.explicitContent);
            modelObject.insert("canEdit", model.canEdit);
            modelObject.insert("displayName", model.displayName);
            if (category == ModelCategory::ImageGeneration)
                modelObject.insert("styles", KiraService::Protocol::promptStylesToJson(model.styles));
            if (!model.recommendedOptions.isEmpty()) modelObject.insert("recommendedOptions", model.recommendedOptions);
            modelObject.insert("companionFiles", model.companionFiles);
            modelObject.insert("modelType", model.modelType);
            if (!model.companionFileName.isEmpty()) {
                modelObject.insert(QStringLiteral("companionFileName"), model.companionFileName);
                modelObject.insert(QStringLiteral("companionRole"), model.companionRole);
            }
            models.append(modelObject);
        }
        categories.append(QJsonObject{{QStringLiteral("category"), ModelCatalog::categoryName(category)}, {QStringLiteral("models"), models}});
    }
    send(socket, {{QStringLiteral("type"), QStringLiteral("models.list.result")}, {QStringLiteral("categories"), categories}});
}

void KiraServiceDaemon::installModel(QWebSocket *socket, const QJsonObject &message)
{
    const QString installId = message.value(QStringLiteral("installId")).toString();
    const std::optional<ModelCategory> category = ModelCatalog::categoryFromName(message.value(QStringLiteral("category")).toString());
    if (installId.isEmpty() || !category.has_value()) {
        send(socket, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("invalid-model-install")},
                      {QStringLiteral("message"), tr("A valid installation ID and model category are required")}, {QStringLiteral("installId"), installId}});
        return;
    }
    if (m_installClients.contains(installId)) {
        send(socket, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("invalid-model-install")},
                      {QStringLiteral("message"), tr("The installation ID is already in use")}, {QStringLiteral("installId"), installId}});
        return;
    }
    std::optional<QUrl> companionUrl;
    if (message.contains(QStringLiteral("companionUrl"))) companionUrl = QUrl(message.value(QStringLiteral("companionUrl")).toString());
    ModelInstallRequest request;
    request.installId = installId;
    if (message.contains("displayName")) {
        if (!message.value("displayName").isString()) {
            send(socket, {{"type", "error"}, {"code", "invalid-model-install"}, {"installId", installId}, {"message", tr("Display name must be a string")}}); return;
        }
        request.displayName = message.value("displayName").toString();
    }
    request.category = *category;
    request.allowLocalFiles = socket->peerAddress().isLoopback();
    request.url = QUrl(message.value("url").toString());
    request.companionUrl = companionUrl;
    request.existingModelId = message.value("id").toString();
    request.modelType = message.value("modelType").toString(request.existingModelId.isEmpty() ? "checkpoint" : "");
    if ((message.contains("companionUrls") && !message.value("companionUrls").isObject())
        || (message.contains("modelType") && !message.value("modelType").isString())
        || (message.contains("id") && !message.value("id").isString())) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-install"}, {"installId", installId},
            {"message", tr("Invalid companion URLs or model type")}}); return;
    }
    if (message.contains("explicitContent")) {
        if (!message.value("explicitContent").isBool()) {
            send(socket, {{"type", "error"}, {"code", "invalid-model-install"}, {"installId", installId},
                {"message", tr("Explicit content must be a boolean")}}); return;
        }
        request.explicitContent = message.value("explicitContent").toBool();
    }
    if (message.contains("canEdit")) {
        if (!message.value("canEdit").isBool()) {
            send(socket, {{"type", "error"}, {"code", "invalid-model-install"}, {"installId", installId},
                {"message", tr("Can edit must be a boolean")}}); return;
        }
        request.canEdit = message.value("canEdit").toBool();
    }
    request.companionUrls = message.value("companionUrls").toObject();
    if (!request.existingModelId.isEmpty()) {
        const auto model = m_models->model(*category, request.existingModelId);
        if (model && modelInUse(model->filePath)) {
            send(socket, {{"type", "error"}, {"code", "model-in-use"}, {"installId", installId},
                {"message", tr("Cannot attach files while this model is in use")}}); return;
        }
    }
    if (m_models->isInstalling()) {
        send(socket, {{"type", "error"}, {"code", "model-install-busy"}, {"message", tr("Another model installation is in progress")}, {"installId", installId}});
        return;
    }
    if (message.contains("recommendedOptions")) {
        if (!message.value("recommendedOptions").isObject()) {
            send(socket, {{"type", "error"}, {"code", "invalid-model-install"}, {"message", tr("Recommended options must be an object")}, {"installId", installId}});
            return;
        }
        request.recommendedOptions = message.value("recommendedOptions").toObject();
    }
    m_installClients.insert(installId, socket);
    QString error;
    if (!m_models->install(request, &error)) {
        m_installClients.remove(installId);
        send(socket, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("invalid-model-install")},
                      {QStringLiteral("message"), error}, {QStringLiteral("installId"), installId}});
        return;
    }
    send(socket, {{QStringLiteral("type"), QStringLiteral("models.install.accepted")}, {QStringLiteral("installId"), installId}});
}

void KiraServiceDaemon::configureModel(QWebSocket *socket, const QJsonObject &message)
{
    const QString id = message.value("id").toString();
    QString error;
    if (message.contains("displayName") && !message.value("displayName").isString()) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-configure"}, {"id", id}, {"message", tr("Display name must be a string")}}); return;
    }
    const std::optional<QString> displayName = message.contains("displayName")
        ? std::optional<QString>(message.value("displayName").toString()) : std::nullopt;
    if (message.contains("explicitContent") && !message.value("explicitContent").isBool()) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-configure"}, {"id", id},
            {"message", tr("Explicit content must be a boolean")}}); return;
    }
    const std::optional<bool> explicitContent = message.contains("explicitContent")
        ? std::optional<bool>(message.value("explicitContent").toBool()) : std::nullopt;
    if (message.contains("canEdit") && !message.value("canEdit").isBool()) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-configure"}, {"id", id},
            {"message", tr("Can edit must be a boolean")}}); return;
    }
    const std::optional<bool> canEdit = message.contains("canEdit")
        ? std::optional<bool>(message.value("canEdit").toBool()) : std::nullopt;
    if (!message.value("recommendedOptions").isObject() || !m_models->configure(id, message.value("recommendedOptions").toObject(), &error, explicitContent, displayName, canEdit)) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-configure"}, {"id", id}, {"message", error.isEmpty() ? tr("Recommended options must be an object") : error}});
        return;
    }
    const auto model = *m_models->modelById(id);
    QJsonObject value{{"id", model.id}, {"name", model.name}, {"fileName", model.fileName}, {"bytes", model.fileSize}, {"recommendedOptions", model.recommendedOptions}};
    value.insert("explicitContent", model.explicitContent);
    value.insert("canEdit", model.canEdit);
    value.insert("displayName", model.displayName);
    value.insert("companionFiles", model.companionFiles);
    value.insert("modelType", model.modelType);
    if (!model.companionFileName.isEmpty()) {
        value.insert("companionFileName", model.companionFileName);
        value.insert("companionRole", model.companionRole);
    }
    send(socket, {{"type", "models.configure.completed"}, {"id", id}, {"model", value}});
    if (canEdit) {
        for (auto it = m_authenticatedClients.cbegin(); it != m_authenticatedClients.cend(); ++it)
            if (it.value()) send(it.key(), {{"type", "models.changed"}, {"id", id}});
    }
}

void KiraServiceDaemon::configureModelStyles(QWebSocket *socket, const QJsonObject &message)
{
    const QString id = message.value("id").toString();
    QString error;
    const auto styles = KiraService::Protocol::promptStylesFromJson(message.value("styles"), &error);
    if (!styles || !m_models->configureStyles(id, *styles, &error)) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-styles"}, {"id", id}, {"message", error}});
        return;
    }
    send(socket, {{"type", "models.styles.configure.completed"}, {"id", id},
        {"styles", KiraService::Protocol::promptStylesToJson(*styles)}});
    for (auto it = m_authenticatedClients.cbegin(); it != m_authenticatedClients.cend(); ++it)
        if (it.value()) send(it.key(), {{"type", "models.styles.changed"}, {"id", id}});
}

void KiraServiceDaemon::cancelInstall(QWebSocket *socket, const QJsonObject &message)
{
    const QString id = message.value("installId").toString();
    if (id.isEmpty() || m_installClients.value(id) != socket) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-cancel"}, {"installId", id}, {"message", tr("This installation is not owned by this connection")}});
        return;
    }
    m_models->cancel(id);
}

void KiraServiceDaemon::deleteModel(QWebSocket *socket, const QJsonObject &message)
{
    const QString id = message.value(QStringLiteral("id")).toString();
    const std::optional<ModelInfo> model = m_models->modelById(id);
    if (!model.has_value()) {
        send(socket, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("invalid-model-delete")},
                     {QStringLiteral("message"), tr("The requested model is not installed")}, {QStringLiteral("id"), id}});
        return;
    }
    if (modelInUse(model->filePath)) {
        send(socket, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("model-in-use")},
                     {QStringLiteral("message"), tr("The model is currently used by queued or active work")}, {QStringLiteral("id"), id}});
        return;
    }
    QString error;
    if (!m_models->remove(id, &error)) {
        send(socket, {{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("model-delete-failed")},
                     {QStringLiteral("message"), error}, {QStringLiteral("id"), id}});
        return;
    }
    send(socket, {{QStringLiteral("type"), QStringLiteral("models.delete.completed")}, {QStringLiteral("id"), id}});
}

bool KiraServiceDaemon::modelInUse(const QString &path) const
{
    const auto usesModel = [&path](const auto &job) {
        return job && (job->modelPath == path || job->clipVisionPath == path || job->ipAdapterPath == path || job->photoMakerPath == path);
    };
    if (usesModel(m_activeJob)) return true;
    for (const auto &job : m_queue) {
        if (usesModel(job)) return true;
    }
    for (auto it = m_pendingUploads.cbegin(); it != m_pendingUploads.cend(); ++it) {
        if (usesModel(it.value())) return true;
    }
    return false;
}

void KiraServiceDaemon::processBinaryMessage(QWebSocket *socket, const QByteArray &data)
{
    const auto submission = m_pendingUploads.value(socket);
    if (!submission) { sendError(socket, QStringLiteral("unexpected-binary"), tr("No image upload was requested")); return; }
    if (data.isEmpty() || data.size() > m_configuration.maximumImageBytes) {
        m_pendingUploads.remove(socket);
        sendError(socket, QStringLiteral("invalid-image"), tr("Image upload is empty or too large"), submission->id);
        return;
    }
    if (submission->operation == Operation::Inpaint && !submission->waitingIpAdapterImage
        && !submission->waitingPhotoMakerReferences && !submission->waitingPhotoMakerIdentity) {
        const auto validPng = [&](const QByteArray& bytes) {
            return bytes.size() >= 33 && bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))
                && bytes.mid(12, 4) == "IHDR"
                && qFromBigEndian<quint32>(bytes.constData() + 8) == 13
                && qFromBigEndian<quint32>(bytes.constData() + 16) == quint32(submission->options.value("width").toInt())
                && qFromBigEndian<quint32>(bytes.constData() + 20) == quint32(submission->options.value("height").toInt());
        };
        if (!validPng(data) || data.size() + submission->inputData.size() > m_configuration.maximumImageBytes) {
            m_pendingUploads.remove(socket);
            sendError(socket, "invalid-image", tr("Source and mask must be PNGs with the requested dimensions and fit the upload limit"), submission->id); return;
        }
        if (!submission->waitingMask) {
            submission->inputData = data;
            submission->waitingMask = true;
            send(socket, {{"type", "upload-required"}, {"jobId", submission->id}, {"inputRole", "mask"}});
            return;
        }
        submission->maskImageData = data;
        if (!submission->photoMakerPath.isEmpty()) {
            submission->waitingPhotoMakerReferences = true;
            send(socket, {{"type", "upload-required"}, {"jobId", submission->id},
                {"inputRole", "photomaker-reference"}, {"referenceIndex", 0}});
            return;
        }
    }
    else if (submission->waitingPhotoMakerIdentity) submission->photoMakerIdentityData = data;
    else if (submission->operation == Operation::PhotoMakerExport || !submission->photoMakerPath.isEmpty()) {
        qsizetype total = data.size();
        if (submission->operation == Operation::Inpaint)
            total += submission->inputData.size() + submission->maskImageData.size();
        for (const auto& image : submission->referenceImages) total += image.size();
        if (total > m_configuration.maximumImageBytes
            || (submission->referenceCount > 1 && !data.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))) {
            m_pendingUploads.remove(socket);
            sendError(socket, "invalid-image", tr("Invalid or oversized reference images"), submission->id); return;
        }
        submission->referenceImages.append(data);
        if (submission->referenceImages.size() < submission->referenceCount) {
            send(socket, {{"type", "upload-required"}, {"jobId", submission->id},
                {"inputRole", "photomaker-reference"}, {"referenceIndex", submission->referenceImages.size()}});
            return;
        }
        if (submission->operation != Operation::Inpaint)
            submission->inputData = submission->referenceImages.first();
    }
    else if (submission->waitingIpAdapterImage) {
        if (submission->operation == Operation::Inpaint
            && data.size() + submission->inputData.size() + submission->maskImageData.size() > m_configuration.maximumImageBytes) {
            m_pendingUploads.remove(socket);
            sendError(socket, "invalid-image", tr("The combined source, mask and reference upload is too large"), submission->id);
            return;
        }
        submission->ipAdapterImageData = data;
    }
    else submission->inputData = data;
    if (!submission->photoMakerPath.isEmpty() && !submission->waitingPhotoMakerIdentity) {
        submission->waitingPhotoMakerIdentity = true;
        send(socket, {{"type", "upload-required"}, {"jobId", submission->id}, {"inputRole", "photomaker-identity"}});
        return;
    }
    if (!submission->waitingIpAdapterImage && !submission->ipAdapterPath.isEmpty()) {
        submission->waitingIpAdapterImage = true;
        send(socket, {{"type", "upload-required"}, {"jobId", submission->id}, {"inputRole", "ip-adapter"}});
        return;
    }
    if (submission->operation == Operation::Inpaint) {
        qsizetype total = submission->inputData.size() + submission->maskImageData.size()
            + submission->ipAdapterImageData.size() + submission->photoMakerIdentityData.size();
        for (const auto& reference : submission->referenceImages) total += reference.size();
        if (total > m_configuration.maximumImageBytes) {
            m_pendingUploads.remove(socket);
            sendError(socket, "invalid-image", tr("The combined inpainting upload is too large"), submission->id);
            return;
        }
    }
    m_pendingUploads.remove(socket);
    if (m_queue.size() >= MaximumQueuedJobs) {
        sendError(socket, "queue-full", tr("The service queue is full"), submission->id);
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = submission->id;
    job->operation = submission->operation;
    job->options = submission->options;
    job->modelPath = submission->modelPath;
    job->vaePath = submission->vaePath;
    job->llmPath = submission->llmPath;
    job->llmVisionPath = submission->llmVisionPath;
    job->clipLPath = submission->clipLPath;
    job->clipGPath = submission->clipGPath;
    job->diffusionModel = submission->diffusionModel;
    job->clipVisionPath = submission->clipVisionPath;
    job->ipAdapterPath = submission->ipAdapterPath;
    job->ipAdapterLoraPath = submission->ipAdapterLoraPath;
    job->client = socket;
    job->photoMakerPath = submission->photoMakerPath;
    job->maskPath = submission->maskImageData.isEmpty() ? QString() : job->directory->filePath("mask.png");
    if (!job->directory->isValid()) { sendError(socket, QStringLiteral("storage-error"), tr("Could not create job storage"), job->id); return; }
    const auto saveImage = [&](const QByteArray &bytes, const QString &name, QString &path) {
        if (bytes.isEmpty()) return true;
        path = job->directory->filePath(name);
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const bool photoMakerInpaint = job->operation == Operation::Inpaint && !job->photoMakerPath.isEmpty();
    if (photoMakerInpaint && !QDir().mkpath(job->directory->filePath("identity"))) {
        sendError(socket, "storage-error", tr("Could not create identity storage"), job->id); return;
    }
    job->photoMakerImagesDirectory = photoMakerInpaint ? job->directory->filePath("identity") : job->directory->path();
    if (!saveImage(submission->inputData, "input.png", job->inputPath)
        || !saveImage(submission->maskImageData, "mask.png", job->maskPath)
        || !saveImage(submission->ipAdapterImageData, "ip-adapter.png", job->ipAdapterImagePath)
        || !saveImage(submission->photoMakerIdentityData, photoMakerInpaint ? "identity/id_embeds.bin" : "id_embeds.bin", job->photoMakerIdentityPath)) {
        sendError(socket, QStringLiteral("storage-error"), tr("Could not save image upload"), job->id);
        return;
    }
    for (int i = photoMakerInpaint ? 0 : 1; i < submission->referenceImages.size(); ++i) {
        QString path;
        const auto name = photoMakerInpaint
            ? QStringLiteral("identity/reference-%1.png").arg(i + 1)
            : QStringLiteral("reference-%1.png").arg(i + 1);
        if (!saveImage(submission->referenceImages[i], name, path)) {
            sendError(socket, "storage-error", tr("Could not save reference image"), job->id); return;
        }
    }
    enqueue(job);
}

void KiraServiceDaemon::enqueue(std::shared_ptr<Job> job)
{
    m_queue.enqueue(job);
    send(job->client, {{QStringLiteral("type"), QStringLiteral("queued")}, {QStringLiteral("jobId"), job->id}});
    broadcastQueuePositions();
    startNextJob();
}

void KiraServiceDaemon::startNextJob()
{
    if (m_activeJob || m_queue.isEmpty()) return;
    m_activeJob = m_queue.dequeue();
    if (m_activeJob->client.isNull()) { m_activeJob.reset(); startNextJob(); return; }
    send(m_activeJob->client, {{QStringLiteral("type"), QStringLiteral("running")}, {QStringLiteral("jobId"), m_activeJob->id}});
    if (m_activeJob->operation == Operation::PromptEnhance) {
        startPromptEnhancement();
    } else if (m_activeJob->operation == Operation::FaceParsing) {
        startFaceParsing();
        return;
    } else if (m_activeJob->operation == Operation::PhotoMakerExport) {
        m_activeJob->outputPath = m_activeJob->directory->filePath("id_embeds.bin");
        QFile wrapper(":/photomaker/photomaker-export.py");
        if (!wrapper.open(QIODevice::ReadOnly)) {
            sendError(m_activeJob->client, "photomaker-extraction-failed", tr("Could not load the identity extractor"), m_activeJob->id);
            m_activeJob.reset();
            startNextJob();
            return;
        }
        m_photoMakerError.clear();
        m_photoMakerTimedOut = false;
        m_photoMakerProcess.setWorkingDirectory(m_activeJob->directory->path());
        m_photoMakerTimeout.start();
        m_photoMakerProcess.start(m_configuration.photoMakerPython,
            {"-c", QString::fromUtf8(wrapper.readAll()), QFileInfo(m_configuration.photoMakerFaceDetect).absoluteFilePath(),
             m_activeJob->directory->path()});
        broadcastQueuePositions();
        return;
    }
    if (m_activeJob->operation == Operation::Generate || m_activeJob->operation == Operation::Edit || m_activeJob->operation == Operation::Inpaint) {
        m_activeJob->outputPath = m_activeJob->directory->filePath(QStringLiteral("result.png"));
        GenerationRequest request;
        const QJsonObject &o = m_activeJob->options;
        request.prompt = o.value(QStringLiteral("prompt")).toString(); request.negativePrompt = o.value(QStringLiteral("negativePrompt")).toString();
        request.sampler = o.value(QStringLiteral("sampler")).toString(); request.scheduler = o.value(QStringLiteral("scheduler")).toString();
        request.width = o.value(QStringLiteral("width")).toInt(request.width);
        request.height = o.value(QStringLiteral("height")).toInt(request.height);
        request.steps = o.value(QStringLiteral("steps")).toInt(request.steps);
        request.cfgScale = o.value(QStringLiteral("cfgScale")).toDouble(7.0); request.clipSkip = o.value(QStringLiteral("clipSkip")).toInt(-1); request.seed = o.value(QStringLiteral("seed")).toInt(-1);
        request.baseImageStrength = o.value(QStringLiteral("baseImageStrength")).toDouble(m_activeJob->operation == Operation::Inpaint ? 0.99 : 0.55); request.outputPath = m_activeJob->outputPath;
        request.modelPath = m_activeJob->modelPath;
        request.vaePath = m_activeJob->vaePath;
        request.llmPath = m_activeJob->llmPath;
        request.llmVisionPath = m_activeJob->llmVisionPath;
        request.clipLPath = m_activeJob->clipLPath;
        request.clipGPath = m_activeJob->clipGPath;
        request.diffusionModel = m_activeJob->diffusionModel;
        request.clipVisionPath = m_activeJob->clipVisionPath;
        request.ipAdapterPath = m_activeJob->ipAdapterPath;
        request.ipAdapterLoraPath = m_activeJob->ipAdapterLoraPath;
        request.ipAdapterImagePath = m_activeJob->ipAdapterImagePath;
        request.ipAdapterStrength = o.value("ip_adapter_strength").toDouble(1.0);
        request.photoMakerPath = m_activeJob->photoMakerPath;
        request.photoMakerIdentityPath = m_activeJob->photoMakerIdentityPath;
        request.maskImagePath = m_activeJob->maskPath;
        request.photoMakerImagesDirectory = m_activeJob->photoMakerImagesDirectory.isEmpty()
            ? m_activeJob->directory->path() : m_activeJob->photoMakerImagesDirectory;
        request.photoMakerStyleStrength = o.value("photoMakerStyleStrength").toInt(20);
        if (m_activeJob->operation == Operation::Edit) request.referenceImagePath = m_activeJob->inputPath;
        else if (o.value(QStringLiteral("hasInput")).toBool()) request.baseImagePath = m_activeJob->inputPath;
        if (!m_sdRunner || !m_sdRunner->runGeneration(request)) { sendError(m_activeJob->client, QStringLiteral("start-failed"), tr("Could not start generation"), m_activeJob->id); m_activeJob.reset(); startNextJob(); }
    } else if (m_activeJob->operation == Operation::Upscale) {
        m_activeJob->outputPath = m_activeJob->directory->filePath(QStringLiteral("result.png"));
        const QStringList arguments = {QStringLiteral("--mode"), QStringLiteral("upscale"), QStringLiteral("--init-img"), m_activeJob->inputPath, QStringLiteral("--upscale-model"), m_activeJob->modelPath, QStringLiteral("--upscale-repeats"), QStringLiteral("1"), QStringLiteral("--upscale-tile-size"), QStringLiteral("128"), QStringLiteral("--output"), m_activeJob->outputPath};
        if (!m_sdRunner || !m_sdRunner->runUpscale(arguments, m_activeJob->outputPath)) { sendError(m_activeJob->client, QStringLiteral("start-failed"), tr("Could not start upscaling"), m_activeJob->id); m_activeJob.reset(); startNextJob(); }
    }
    broadcastQueuePositions();
}

void KiraServiceDaemon::handleGenerationFinished(const SdCliGenerationResult &result)
{
    if (!m_activeJob) return;
    const auto job = m_activeJob;
    if (!result.succeeded || !QFileInfo(job->outputPath).isFile()) {
        QString detail = tr("Image generation failed (exit code %1)").arg(result.exitCode);
        detail += tr("\nCommand:\n%1").arg(m_sdRunner->commandLine(result.arguments));
        const QString diagnostic = result.standardError.trimmed().right(4096);
        if (!diagnostic.isEmpty()) detail += tr("\nBackend error:\n%1").arg(diagnostic);
        sendError(job->client, QStringLiteral("generation-failed"), detail, job->id);
    }
    else { QFile output(job->outputPath); const bool opened = output.open(QIODevice::ReadOnly); const QByteArray bytes = opened ? output.readAll() : QByteArray(); if (bytes.isEmpty()) sendError(job->client, QStringLiteral("result-read-failed"), tr("Could not read generated image"), job->id); else { send(job->client, {{QStringLiteral("type"), QStringLiteral("result")}, {QStringLiteral("jobId"), job->id}, {QStringLiteral("operation"), job->operation == Operation::Inpaint ? QStringLiteral("image.inpaint") : job->operation == Operation::Edit ? QStringLiteral("image.edit") : QStringLiteral("image.generate")}, {QStringLiteral("mimeType"), QStringLiteral("image/png")}}); job->client->sendBinaryMessage(bytes); } }
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
}

void KiraServiceDaemon::handleUpscaleFinished(const SdCliUpscaleResult &result)
{
    if (!m_activeJob) return;
    const auto job = m_activeJob;
    if (!result.succeeded || !QFileInfo(job->outputPath).isFile()) sendError(job->client, QStringLiteral("upscale-failed"), tr("Image upscaling failed"), job->id);
    else { QFile output(job->outputPath); const bool opened = output.open(QIODevice::ReadOnly); const QByteArray bytes = opened ? output.readAll() : QByteArray(); if (bytes.isEmpty()) sendError(job->client, QStringLiteral("result-read-failed"), tr("Could not read upscaled image"), job->id); else { send(job->client, {{QStringLiteral("type"), QStringLiteral("result")}, {QStringLiteral("jobId"), job->id}, {QStringLiteral("operation"), QStringLiteral("image.upscale")}, {QStringLiteral("mimeType"), QStringLiteral("image/png")}}); job->client->sendBinaryMessage(bytes); } }
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
}

void KiraServiceDaemon::cancelJobsFor(QWebSocket *socket)
{
    QQueue<std::shared_ptr<Job>> kept;
    while (!m_queue.isEmpty()) { const auto job = m_queue.dequeue(); if (job->client != socket) kept.enqueue(job); }
    m_queue = std::move(kept);
    if (m_activeJob && m_activeJob->client == socket) {
        if (m_activeJob->operation == Operation::PromptEnhance) m_promptProcess.kill();
        else if (m_activeJob->operation == Operation::FaceParsing) m_faceParsingProcess.kill();
        else if (m_activeJob->operation == Operation::PhotoMakerExport) m_photoMakerProcess.kill();
        else if (m_sdRunner) m_sdRunner->cancel();
    }
    broadcastQueuePositions();
}

void KiraServiceDaemon::broadcastQueuePositions()
{
    for (int i = 0; i < m_queue.size(); ++i) send(m_queue.at(i)->client, {{QStringLiteral("type"), QStringLiteral("queue-position")}, {QStringLiteral("jobId"), m_queue.at(i)->id}, {QStringLiteral("position"), i + 1}});
}

void KiraServiceDaemon::sendCapabilities(QWebSocket *socket) const
{
    const QString state = isImageReady() || isPhotoMakerExportReady() || isPromptEnhancementReady() || isFaceParsingReady() ? QStringLiteral("ready") : QStringLiteral("unavailable");
    QJsonArray operations;
    if (isImageReady()) {
        operations.append(QStringLiteral("image.generate"));
        operations.append(QStringLiteral("image.upscale"));
        operations.append(QStringLiteral("image.edit"));
        operations.append(QStringLiteral("image.inpaint"));
    }
    if (isPhotoMakerExportReady()) operations.append("photomaker.export");
    if (isPromptEnhancementReady()) operations.append("prompt.enhance");
    if (isFaceParsingReady()) operations.append("image.face-parsing");
    send(socket, {{QStringLiteral("type"), QStringLiteral("capabilities")}, {QStringLiteral("state"), state}, {QStringLiteral("operations"), operations}, {"ipAdapter", isImageReady()}, {"photoMaker", isImageReady()}});
}

bool KiraServiceDaemon::isFaceParsingReady() const
{
    return !m_configuration.faceParsingPython.isEmpty()
        && !QStandardPaths::findExecutable(m_configuration.faceParsingPython).isEmpty();
}

void KiraServiceDaemon::startFaceParsing()
{
    auto job = m_activeJob;
    job->outputPath = job->directory->filePath("mask.png");
    QFile wrapper(":/face-parsing/face-parsing.py");
    if (!wrapper.open(QIODevice::ReadOnly)) {
        sendError(job->client, "face-parsing-failed", tr("Could not load the face parser"), job->id);
        m_activeJob.reset(); startNextJob(); return;
    }
    m_faceParsingError.clear();
    m_faceParsingTimedOut = false;
    m_faceParsingProcess.setWorkingDirectory(job->directory->path());
    m_faceParsingTimeout.start();
    m_faceParsingProcess.start(m_configuration.faceParsingPython,
        {"-c", QString::fromUtf8(wrapper.readAll()), job->modelPath, job->inputPath, job->outputPath,
         QString::fromUtf8(QJsonDocument(job->options).toJson(QJsonDocument::Compact))});
    broadcastQueuePositions();
}

void KiraServiceDaemon::handleFaceParsingFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_activeJob || m_activeJob->operation != Operation::FaceParsing) return;
    m_faceParsingTimeout.stop();
    const auto job = m_activeJob;
    m_faceParsingError = (m_faceParsingError + m_faceParsingProcess.readAllStandardError()).right(4096);
    QFile output(job->outputPath);
    QByteArray bytes;
    if (status == QProcess::NormalExit && exitCode == 0 && !m_faceParsingTimedOut
        && output.size() <= m_configuration.maximumImageBytes && output.open(QIODevice::ReadOnly))
        bytes = output.readAll();
    if (!bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
        QString detail = m_faceParsingTimedOut ? tr("Face parsing timed out") : tr("Face parsing failed");
        if (!m_faceParsingError.isEmpty()) detail += '\n' + QString::fromUtf8(m_faceParsingError);
        sendError(job->client, "face-parsing-failed", detail, job->id);
    } else if (job->client) {
        send(job->client, {{"type", "result"}, {"jobId", job->id},
            {"operation", "image.face-parsing"}, {"mimeType", "image/png"}});
        job->client->sendBinaryMessage(bytes);
    }
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
}

bool KiraServiceDaemon::isPromptEnhancementReady() const
{
    return !m_configuration.llamaCompletion.isEmpty()
        && !QStandardPaths::findExecutable(m_configuration.llamaCompletion).isEmpty();
}

void KiraServiceDaemon::submitPromptEnhancement(QWebSocket* socket, const QJsonObject& message)
{
    const auto id = message.value("jobId").toString();
    const auto request = message.value("options").toObject();
    if (!message.value("options").isObject() || !request.value("modelId").isString()
        || !request.value("prompt").isString()
        || request.value("prompt").toString().toUtf8().size() > 65536
        || (request.contains("parameters") && !request.value("parameters").isObject())) {
        sendError(socket, "invalid-request", tr("Prompt enhancement requires a model and a prompt of at most 64 KiB"), id);
        return;
    }
    for (const auto& key : request.keys()) if (key != "modelId" && key != "prompt" && key != "parameters") {
        sendError(socket, "invalid-request", tr("Unsupported prompt enhancement field"), id); return;
    }
    const auto model = m_models->model(ModelCategory::PromptEnhancer, request.value("modelId").toString());
    if (!model) { sendError(socket, "model-unavailable", tr("The requested Prompt Enhancer model is unavailable"), id); return; }
    auto parameters = model->recommendedOptions.isEmpty()
        ? KiraService::Protocol::defaultPromptEnhancerOptions("LLM") : model->recommendedOptions;
    if (parameters.value("subtype") != "LLM") {
        sendError(socket, "prompt-enhancer-subtype-unsupported", tr("Only LLM prompt enhancement is supported; T5 and VLM are not implemented"), id); return;
    }
    const auto overrides = request.value("parameters").toObject();
    for (const auto& key : overrides.keys()) {
        if (key == "subtype" || !parameters.contains(key)) {
            sendError(socket, "invalid-request", tr("Unsupported Prompt Enhancer parameter"), id); return;
        }
        parameters.insert(key, overrides.value(key));
    }
    if (!KiraService::Protocol::validateModelOptions("prompt-enhancer", parameters)
        || parameters.value("systemPrompt").toString().toUtf8().size() > 65536) {
        sendError(socket, "invalid-request", tr("Invalid Prompt Enhancer parameters"), id); return;
    }
    if (QFileInfo(model->filePath).suffix().toLower() != "gguf") {
        sendError(socket, "prompt-enhancer-format-unsupported", tr("llama-completion requires a compatible GGUF model"), id); return;
    }
    if (!isPromptEnhancementReady()) {
        sendError(socket, "backend-not-ready", tr("llama-completion is unavailable"), id); return;
    }
    if (m_pendingUploads.contains(socket) || m_queue.size() >= MaximumQueuedJobs) {
        sendError(socket, "queue-full", tr("An upload is pending or the service queue is full"), id); return;
    }
    auto job = std::make_shared<Job>();
    job->id = id;
    job->operation = Operation::PromptEnhance;
    job->client = socket;
    job->modelPath = model->filePath;
    job->options = parameters;
    job->options.insert("prompt", request.value("prompt"));
    enqueue(job);
}

void KiraServiceDaemon::startPromptEnhancement()
{
    const auto job = m_activeJob;
    const auto parameters = job->options;
    const QString promptPath = job->directory->filePath("prompt.txt");
    const QString systemPath = job->directory->filePath("system.txt");
    const auto write = [](const QString& path, const QString& text) {
        QFile file(path);
        const auto bytes = text.toUtf8();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QString system = parameters.value("systemPrompt").toString();
    if (system.trimmed().isEmpty())
        system = QStringLiteral("Enhance the user's image-generation prompt. Return only the enhanced prompt, without explanations, headings, quotation marks or reasoning.");
    const auto input = parameters.value("prompt").toString();
    if (!job->directory->isValid() || !write(promptPath, input.trimmed().isEmpty()
        ? QStringLiteral("Generate a new description according to the system instructions.") : input) || !write(systemPath, system)) {
        sendError(job->client, "storage-error", tr("Could not prepare prompt enhancement"), job->id);
        m_activeJob.reset(); startNextJob(); return;
    }
    QStringList arguments{"--model", job->modelPath, "--file", promptPath, "--system-prompt-file", systemPath,
        "--single-turn", "--conversation", "--jinja", "--no-display-prompt", "--simple-io", "--color", "off",
        "--reasoning", "off", "--no-context-shift", "--ctx-size", QString::number(parameters.value("contextLength").toInt()),
        "--n-predict", QString::number(parameters.value("maxOutputTokens").toInt()),
        "--repeat-penalty", QString::number(parameters.value("repetitionPenalty").toDouble(), 'g', 17),
        "--repeat-last-n", QString::number(parameters.value("contextLength").toInt()),
        "--samplers", "penalties;top_k;top_p;temperature", "--temp",
        parameters.value("doSample").toBool() ? QString::number(parameters.value("temperature").toDouble(), 'g', 17) : QStringLiteral("0"),
        "--top-p", QString::number(parameters.value("topP").toDouble(), 'g', 17),
        "--top-k", QString::number(parameters.value("topK").toInt())};
    m_promptOutput.clear(); m_promptError.clear();
    m_promptTimedOut = false; m_promptOutputExceeded = false;
    m_promptTimeout.start();
    m_promptProcess.start(m_configuration.llamaCompletion, arguments);
}

void KiraServiceDaemon::handlePromptEnhancementFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_activeJob || m_activeJob->operation != Operation::PromptEnhance) return;
    m_promptTimeout.stop();
    const auto job = m_activeJob;
    m_promptError = (m_promptError + m_promptProcess.readAllStandardError()).right(4096);
    const auto remaining = m_promptProcess.readAllStandardOutput();
    if (m_promptOutput.size() + remaining.size() > 1024 * 1024) m_promptOutputExceeded = true;
    else m_promptOutput += remaining;
    QString prompt = QString::fromUtf8(m_promptOutput).trimmed();
    const QString endMarker = QStringLiteral("[end of text]");
    if (prompt.endsWith(endMarker)) prompt.chop(endMarker.size());
    prompt = prompt.trimmed();
    if (status != QProcess::NormalExit || exitCode != 0 || m_promptTimedOut || m_promptOutputExceeded || prompt.isEmpty()) {
        const QString code = m_promptTimedOut ? "prompt-enhancement-timeout" : m_promptOutputExceeded
            ? "prompt-enhancement-output-too-large" : prompt.isEmpty() && exitCode == 0
                ? "prompt-enhancement-empty-result" : "prompt-enhancement-failed";
        QString detail = tr("Prompt enhancement failed (exit code %1)").arg(exitCode);
        if (!m_promptError.trimmed().isEmpty()) detail += QLatin1Char('\n') + QString::fromUtf8(m_promptError).trimmed();
        sendError(job->client, code, detail, job->id);
    } else send(job->client, {{"type", "result"}, {"jobId", job->id}, {"operation", "prompt.enhance"}, {"prompt", prompt}});
    m_promptOutput.clear(); m_promptError.clear();
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
}

bool KiraServiceDaemon::isPhotoMakerExportReady() const
{
    return QFileInfo(m_configuration.photoMakerFaceDetect).isFile()
        && !QStandardPaths::findExecutable(m_configuration.photoMakerPython).isEmpty();
}

void KiraServiceDaemon::handlePhotoMakerExportFinished(int exitCode, QProcess::ExitStatus status)
{
    m_photoMakerTimeout.stop();
    if (!m_activeJob || m_activeJob->operation != Operation::PhotoMakerExport) return;
    const auto job = m_activeJob;
    m_photoMakerError = (m_photoMakerError + m_photoMakerProcess.readAllStandardError()).right(4096);
    QString code;
    QString detail;
    if (m_photoMakerTimedOut || status != QProcess::NormalExit || exitCode != 0) {
        code = status == QProcess::NormalExit && exitCode == 10 ? "photomaker-no-face"
            : status == QProcess::NormalExit && exitCode == 11 ? "photomaker-multiple-faces" : "photomaker-extraction-failed";
        detail = code == "photomaker-no-face" ? tr("No face detected in the source image")
            : code == "photomaker-multiple-faces" ? tr("Multiple faces detected; provide a crop containing one face")
            : m_photoMakerTimedOut ? tr("PhotoMaker identity extraction timed out") : tr("PhotoMaker identity extraction failed");
        if (!m_photoMakerError.isEmpty()) detail += "\n" + QString::fromUtf8(m_photoMakerError);
    } else {
        QFile output(job->outputPath);
        if (!output.exists()) {
            code = "photomaker-output-missing";
            detail = tr("The extractor did not produce id_embeds.bin");
        } else if (!output.open(QIODevice::ReadOnly) || output.size() <= 0 || output.size() > m_configuration.maximumImageBytes) {
            code = "photomaker-extraction-failed";
            detail = tr("Could not read a valid identity export");
        } else {
            const QByteArray bytes = output.readAll();
            if (bytes.size() != output.size()) {
                code = "photomaker-extraction-failed";
                detail = tr("Could not read the identity export");
            } else if (job->client) {
                send(job->client, {{"type", "result"}, {"jobId", job->id}, {"operation", "photomaker.export"},
                    {"mimeType", "application/octet-stream"}, {"fileName", "id_embeds.bin"}});
                job->client->sendBinaryMessage(bytes);
            }
        }
    }
    if (!code.isEmpty()) sendError(job->client, code, detail, job->id);
    m_activeJob.reset();
    broadcastQueuePositions();
    startNextJob();
}

void KiraServiceDaemon::send(QWebSocket *socket, QJsonObject object) const
{
    if (socket) socket->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KiraServiceDaemon::sendError(QWebSocket *socket, const QString &code, const QString &message, const QString &jobId) const
{
    QJsonObject event{{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    if (!jobId.isEmpty()) event.insert(QStringLiteral("jobId"), jobId);
    send(socket, event);
}

std::shared_ptr<KiraServiceDaemon::Job> KiraServiceDaemon::jobForId(const QString &id) const
{
    if (m_activeJob && m_activeJob->id == id) return m_activeJob;
    for (const auto &job : m_queue) if (job->id == id) return job;
    return {};
}

void KiraServiceDaemon::deleteCompanion(QWebSocket* socket, const QJsonObject& message)
{
    const QString id = message.value("id").toString();
    const QString role = message.value("role").toString();
    const auto model = m_models->modelById(id);
    QString error;
    QString code = "model-companion-delete-failed";
    if (model && modelInUse(model->filePath)) {
        code = "model-in-use";
        error = tr("The model is currently used by queued or active work");
    } else if (m_models->removeCompanion(id, role, &error)) {
        send(socket, {{"type", "models.companion.delete.completed"}, {"id", id}, {"role", role}});
        return;
    }
    send(socket, {{"type", "error"}, {"code", code}, {"id", id}, {"role", role}, {"message", error}});
}
