#include "KiraService.h"

#include "GenerationRequest.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include <kira/service/ServiceProtocol.h>
#include "SdCliRunner.h"

#include <QDebug>
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

bool validPositiveInteger(const QJsonObject &object, const QString &name, int minimum, int maximum)
{
    const int value = object.value(name).toInt(-1);
    return value >= minimum && value <= maximum;
}
}

struct KiraServiceDaemon::Submission
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    QString modelPath;
    QPointer<QWebSocket> client;
};

struct KiraServiceDaemon::Job
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    QString modelPath;
    QPointer<QWebSocket> client;
    std::unique_ptr<QTemporaryDir> directory = std::make_unique<QTemporaryDir>();
    QString inputPath;
    QString outputPath;
};

KiraServiceDaemon::KiraServiceDaemon(KiraServiceConfiguration configuration, QObject *parent)
    : QObject(parent)
    , m_configuration(std::move(configuration))
    , m_models(m_configuration.modelRootPath.isEmpty()
                   ? std::make_unique<ModelCatalog>()
                   : std::make_unique<ModelCatalog>(m_configuration.modelRootPath, false))
{
    connect(m_models.get(), &ModelCatalog::installProgress, this, [this](const ModelInstallProgress &progress) {
        const QPointer<QWebSocket> client = m_installClients.value(progress.installId);
        QJsonObject event{{QStringLiteral("type"), QStringLiteral("models.install.progress")},
                          {QStringLiteral("installId"), progress.installId},
                          {QStringLiteral("state"), progress.state == ModelInstallState::Queued ? QStringLiteral("queued")
                                                       : (progress.state == ModelInstallState::Downloading ? QStringLiteral("downloading") : QStringLiteral("verifying"))},
                          {QStringLiteral("bytesDownloaded"), progress.bytesDownloaded}};
        if (progress.totalBytes.has_value()) event.insert(QStringLiteral("totalBytes"), *progress.totalBytes);
        send(client, event);
    });
    connect(m_models.get(), &ModelCatalog::installFinished, this, [this](const QString &installId, const ModelInfo &model) {
        const QPointer<QWebSocket> client = m_installClients.take(installId);
        send(client, {{QStringLiteral("type"), QStringLiteral("models.install.completed")},
                      {QStringLiteral("installId"), installId},
                      {QStringLiteral("model"), QJsonObject{{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.name}, {QStringLiteral("fileName"), model.fileName}, {QStringLiteral("bytes"), model.fileSize}}}});
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
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("sd-cli"));
    if (executable.isEmpty()) {
        qCritical().noquote() << QStringLiteral("[Kira Service] sd-cli was not found on the service PATH.");
        m_backendUnavailable = true;
        return;
    }
    m_runner = std::make_unique<SdCliRunner>(executable);
    connect(m_runner.get(), &SdCliRunner::generationProgressChanged, this, [this](const KiraService::GenerationProgress &progress) {
        if (m_activeJob) send(m_activeJob->client, {{QStringLiteral("type"), QStringLiteral("progress")}, {QStringLiteral("jobId"), m_activeJob->id}, {QStringLiteral("progress"), KiraService::Protocol::progressToJson(progress)}});
    });
    connect(m_runner.get(), &SdCliRunner::generationOutput, this, [this](const QString &stream, const QString &text) {
        if (m_activeJob) send(m_activeJob->client, {{QStringLiteral("type"), QStringLiteral("output")}, {QStringLiteral("jobId"), m_activeJob->id}, {QStringLiteral("stream"), stream}, {QStringLiteral("text"), text}});
    });
    connect(m_runner.get(), &SdCliRunner::generationFinished, this, &KiraServiceDaemon::handleGenerationFinished);
    connect(m_runner.get(), &SdCliRunner::upscaleFinished, this, &KiraServiceDaemon::handleUpscaleFinished);
}

bool KiraServiceDaemon::isReady() const
{
    return !m_backendUnavailable && m_runner != nullptr;
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
    else if (type == QStringLiteral("models.delete")) deleteModel(socket, object);
    else sendError(socket, QStringLiteral("unsupported-message"), tr("Unsupported message"));
}

void KiraServiceDaemon::submit(QWebSocket *socket, const QJsonObject &message)
{
    const QString operationName = message.value(QStringLiteral("operation")).toString();
    const bool generate = operationName == QStringLiteral("image.generate");
    const bool upscale = operationName == QStringLiteral("image.upscale");
    const QJsonObject options = message.value(QStringLiteral("options")).toObject();
    const QString jobId = message.value(QStringLiteral("jobId")).toString();
    bool duplicateId = jobForId(jobId) != nullptr;
    for (const auto &pending : m_pendingUploads) {
        duplicateId = duplicateId || pending->id == jobId;
    }
    if ((!generate && !upscale) || jobId.isEmpty() || duplicateId) { sendError(socket, QStringLiteral("invalid-request"), tr("Invalid job request")); return; }
    if (!isReady()) { sendError(socket, QStringLiteral("backend-not-ready"), tr("The service backend is unavailable"), jobId); return; }
    const ModelCategory category = generate ? ModelCategory::ImageGeneration : ModelCategory::Upscaler;
    const std::optional<ModelInfo> model = m_models->model(category, options.value(QStringLiteral("modelId")).toString());
    if (!model.has_value()) { sendError(socket, QStringLiteral("model-unavailable"), tr("The requested model is unavailable"), jobId); return; }
    if (upscale && options.contains(QStringLiteral("scale"))) {
        sendError(socket, QStringLiteral("invalid-request"), tr("Upscale is fixed at 2×"), jobId);
        return;
    }
    if (m_queue.size() >= MaximumQueuedJobs) { sendError(socket, QStringLiteral("queue-full"), tr("The service queue is full"), jobId); return; }
    if (generate && (options.value(QStringLiteral("prompt")).toString().trimmed().isEmpty()
        || !validPositiveInteger(options, QStringLiteral("width"), 64, 4096)
        || !validPositiveInteger(options, QStringLiteral("height"), 64, 4096)
        || !validPositiveInteger(options, QStringLiteral("steps"), 1, 200))) { sendError(socket, QStringLiteral("invalid-request"), tr("Invalid generation options"), jobId); return; }
    const bool needsInput = upscale || options.value(QStringLiteral("hasInput")).toBool();
    auto submission = std::make_shared<Submission>();
    submission->id = jobId;
    submission->operation = generate ? Operation::Generate : Operation::Upscale;
    submission->options = options;
    submission->modelPath = model->filePath;
    submission->client = socket;
    if (needsInput) {
        m_pendingUploads.insert(socket, submission);
        send(socket, {{QStringLiteral("type"), QStringLiteral("upload-required")}, {QStringLiteral("jobId"), jobId}});
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = jobId;
    job->operation = submission->operation;
    job->options = options;
    job->modelPath = model->filePath;
    job->client = socket;
    enqueue(job);
}

void KiraServiceDaemon::listModels(QWebSocket *socket) const
{
    QJsonArray categories;
    for (const ModelCategory category : ModelCatalog::categories()) {
        QJsonArray models;
        for (const ModelInfo &model : m_models->models(category)) {
            models.append(QJsonObject{{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.name},
                                      {QStringLiteral("fileName"), model.fileName}, {QStringLiteral("bytes"), model.fileSize}});
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
    std::optional<QString> sha256;
    if (message.contains(QStringLiteral("sha256"))) sha256 = message.value(QStringLiteral("sha256")).toString();
    ModelInstallRequest request{installId, *category, QUrl(message.value(QStringLiteral("url")).toString()), sha256};
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
    if (m_activeJob && m_activeJob->modelPath == path) return true;
    for (const auto &job : m_queue) {
        if (job->modelPath == path) return true;
    }
    for (auto it = m_pendingUploads.cbegin(); it != m_pendingUploads.cend(); ++it) {
        if (it.value() && it.value()->modelPath == path) return true;
    }
    return false;
}

void KiraServiceDaemon::processBinaryMessage(QWebSocket *socket, const QByteArray &data)
{
    const auto submission = m_pendingUploads.take(socket);
    if (!submission) { sendError(socket, QStringLiteral("unexpected-binary"), tr("No image upload was requested")); return; }
    if (data.isEmpty() || data.size() > m_configuration.maximumImageBytes) { sendError(socket, QStringLiteral("invalid-image"), tr("Image upload is too large"), submission->id); return; }
    auto job = std::make_shared<Job>();
    job->id = submission->id;
    job->operation = submission->operation;
    job->options = submission->options;
    job->modelPath = submission->modelPath;
    job->client = socket;
    if (!job->directory->isValid()) { sendError(socket, QStringLiteral("storage-error"), tr("Could not create job storage"), job->id); return; }
    job->inputPath = job->directory->filePath(QStringLiteral("input.png"));
    QFile file(job->inputPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) { sendError(socket, QStringLiteral("storage-error"), tr("Could not save image upload"), job->id); return; }
    file.close();
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
    if (m_activeJob || m_queue.isEmpty() || !m_runner) return;
    m_activeJob = m_queue.dequeue();
    if (m_activeJob->client.isNull()) { m_activeJob.reset(); startNextJob(); return; }
    m_activeJob->outputPath = m_activeJob->directory->filePath(QStringLiteral("result.png"));
    send(m_activeJob->client, {{QStringLiteral("type"), QStringLiteral("running")}, {QStringLiteral("jobId"), m_activeJob->id}});
    if (m_activeJob->operation == Operation::Generate) {
        GenerationRequest request;
        const QJsonObject &o = m_activeJob->options;
        request.prompt = o.value(QStringLiteral("prompt")).toString(); request.negativePrompt = o.value(QStringLiteral("negativePrompt")).toString();
        request.sampler = o.value(QStringLiteral("sampler")).toString(); request.scheduler = o.value(QStringLiteral("scheduler")).toString();
        request.width = o.value(QStringLiteral("width")).toInt(); request.height = o.value(QStringLiteral("height")).toInt(); request.steps = o.value(QStringLiteral("steps")).toInt();
        request.cfgScale = o.value(QStringLiteral("cfgScale")).toDouble(); request.seed = o.value(QStringLiteral("seed")).toInt();
        request.baseImageStrength = o.value(QStringLiteral("baseImageStrength")).toDouble(0.55); request.outputPath = m_activeJob->outputPath;
        request.modelPath = m_activeJob->modelPath;
        if (o.value(QStringLiteral("hasInput")).toBool()) request.baseImagePath = m_activeJob->inputPath;
        if (!m_runner->runGeneration(request)) { sendError(m_activeJob->client, QStringLiteral("start-failed"), tr("Could not start generation"), m_activeJob->id); m_activeJob.reset(); startNextJob(); }
    } else {
        const QStringList arguments = {QStringLiteral("--mode"), QStringLiteral("upscale"), QStringLiteral("--init-img"), m_activeJob->inputPath, QStringLiteral("--upscale-model"), m_activeJob->modelPath, QStringLiteral("--upscale-repeats"), QStringLiteral("1"), QStringLiteral("--upscale-tile-size"), QStringLiteral("128"), QStringLiteral("--output"), m_activeJob->outputPath};
        if (!m_runner->runUpscale(arguments, m_activeJob->outputPath)) { sendError(m_activeJob->client, QStringLiteral("start-failed"), tr("Could not start upscaling"), m_activeJob->id); m_activeJob.reset(); startNextJob(); }
    }
    broadcastQueuePositions();
}

void KiraServiceDaemon::handleGenerationFinished(const SdCliGenerationResult &result)
{
    if (!m_activeJob) return;
    const auto job = m_activeJob;
    if (!result.succeeded || !QFileInfo(job->outputPath).isFile()) sendError(job->client, QStringLiteral("generation-failed"), tr("Image generation failed"), job->id);
    else { QFile output(job->outputPath); const bool opened = output.open(QIODevice::ReadOnly); const QByteArray bytes = opened ? output.readAll() : QByteArray(); if (bytes.isEmpty()) sendError(job->client, QStringLiteral("result-read-failed"), tr("Could not read generated image"), job->id); else { send(job->client, {{QStringLiteral("type"), QStringLiteral("result")}, {QStringLiteral("jobId"), job->id}, {QStringLiteral("operation"), QStringLiteral("image.generate")}, {QStringLiteral("mimeType"), QStringLiteral("image/png")}}); job->client->sendBinaryMessage(bytes); } }
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
    if (m_activeJob && m_activeJob->client == socket && m_runner) m_runner->cancel();
    broadcastQueuePositions();
}

void KiraServiceDaemon::broadcastQueuePositions()
{
    for (int i = 0; i < m_queue.size(); ++i) send(m_queue.at(i)->client, {{QStringLiteral("type"), QStringLiteral("queue-position")}, {QStringLiteral("jobId"), m_queue.at(i)->id}, {QStringLiteral("position"), i + 1}});
}

void KiraServiceDaemon::sendCapabilities(QWebSocket *socket) const
{
    const QString state = isReady() ? QStringLiteral("ready") : QStringLiteral("unavailable");
    QJsonArray operations;
    if (isReady()) {
        operations.append(QStringLiteral("image.generate"));
        operations.append(QStringLiteral("image.upscale"));
    }
    send(socket, {{QStringLiteral("type"), QStringLiteral("capabilities")}, {QStringLiteral("state"), state}, {QStringLiteral("operations"), operations}});
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
