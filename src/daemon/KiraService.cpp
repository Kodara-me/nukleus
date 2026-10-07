#include "KiraService.h"

#include "GenerationRequest.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include <kira/service/ServiceProtocol.h>
#include "SdCliRunner.h"

#include <QDebug>
#include <QDir>
#include <cmath>
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
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
constexpr int MaximumEditImages = 8;

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
    const QStringList allowed{"modelId", "prompt", "negativePrompt", "width", "height",
        "steps", "cfgScale", "clipSkip", "seed", "sampler", "scheduler",
        "hasInput", "inputCount", "baseImageStrength", "styleId", "presetId", "qualityId"};
    for (const auto& key : options.keys())
        if (!allowed.contains(key)) return false;
    const auto optionalString = [&options](const QString &key) {
        return !options.contains(key) || options.value(key).isString();
    };
    const auto validChoice = [&options](const QString &key, const QStringList &choices) {
        return !options.contains(key) || (options.value(key).isString() && choices.contains(options.value(key).toString()));
    };
    return options.value("prompt").isString() && !options.value("prompt").toString().trimmed().isEmpty()
        && optionalString("negativePrompt")
        && validNumber(options, "inputCount", 1, MaximumEditImages, true, false)
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

struct PromptFlowJobStep
{
    QString name;
    QString nameFr;
    QString modelPath;
    QJsonObject parameters;
    QString instructions;
};

std::optional<std::pair<QString, QString>> parsePromptFlowOutput(const QString &text, const QString &previousNegative)
{
    // Models sometimes wrap the object in code fences or prose; keep the outermost object.
    const auto start = text.indexOf(QLatin1Char('{'));
    const auto end = text.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end < start) return std::nullopt;
    const auto document = QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8());
    if (!document.isObject()) return std::nullopt;
    const auto object = document.object();
    const auto positive = object.value("positive");
    const auto negative = object.value("negative");
    if (!positive.isString() || positive.toString().trimmed().isEmpty()
        || (object.contains("negative") && !negative.isString())) return std::nullopt;
    return std::pair{positive.toString().trimmed(),
                     object.contains("negative") ? negative.toString().trimmed() : previousNegative};
}

}

// A prompt flow's LLM steps, run in order. For routed generation, preset prefixes
// are then composed once onto the final prompts before the image is generated.
struct KiraServiceDaemon::PromptFlowRun
{
    bool routed = false;
    QList<PromptFlowJobStep> steps;
    int index = 0;
    QString positivePrefix;
    QString negativePrefix;
    QString prompt;
    QString negativePrompt;
    bool active() const { return index < steps.size(); }
};

struct KiraServiceDaemon::Submission
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    PromptFlowRun flow;
    QString modelPath;
    QString vaePath;
    QString llmPath;
    QString llmVisionPath;
    QString clipLPath;
    QString clipGPath;
    bool diffusionModel = false;
    QPointer<QWebSocket> client;
    int expectedInputs = 1;
    QList<QByteArray> inputs;
};

struct KiraServiceDaemon::Job
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    PromptFlowRun flow;
    QString modelPath;
    QString vaePath;
    QString llmPath;
    QString llmVisionPath;
    QString clipLPath;
    QString clipGPath;
    bool diffusionModel = false;
    QPointer<QWebSocket> client;
    std::unique_ptr<QTemporaryDir> directory = std::make_unique<QTemporaryDir>();
    QStringList inputPaths;
    QString outputPath;
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
        if (model.id.startsWith("image-generation:") || model.id.startsWith("llm-chat:"))
            modelObject.insert("explicitContent", model.explicitContent);
        modelObject.insert("displayName", model.displayName);
        modelObject.insert("mmprojModelId", model.mmprojModelId);
        if (!model.recommendedOptions.isEmpty()) modelObject.insert("recommendedOptions", model.recommendedOptions);
        modelObject.insert("companionFiles", model.companionFiles);
        modelObject.insert("modelType", model.modelType);
        if (!model.companionFileName.isEmpty()) {
            modelObject.insert(QStringLiteral("companionFileName"), model.companionFileName);
            modelObject.insert(QStringLiteral("companionRole"), model.companionRole);
        }
        send(client, {{QStringLiteral("type"), QStringLiteral("models.install.completed")},
                      {QStringLiteral("installId"), installId}, {QStringLiteral("model"), modelObject}});
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
    m_promptProcess.disconnect(this);
    m_promptProcess.kill();
    m_promptProcess.waitForFinished(3000);
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
    else if (type == "model-routing.get" || type == "model-routing.configure") modelRouting(socket, object);
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
    const bool promptEnhance = operationName == QStringLiteral("prompt.enhance");
    const bool promptFlow = operationName == QStringLiteral("prompt-flow.run");
    QJsonObject options = message.value(QStringLiteral("options")).toObject();
    const QString jobId = message.value(QStringLiteral("jobId")).toString();
    bool duplicateId = jobForId(jobId) != nullptr;
    for (const auto &pending : m_pendingUploads) {
        duplicateId = duplicateId || pending->id == jobId;
    }
    if ((!generate && !upscale && !edit && !promptEnhance && !promptFlow) || jobId.isEmpty() || duplicateId) { sendError(socket, QStringLiteral("invalid-request"), tr("Invalid job request"), jobId); return; }
    if (promptEnhance) { submitPromptEnhancement(socket, message); return; }
    if (promptFlow) { submitPromptFlow(socket, message); return; }
    if (!isImageReady()) {
        sendError(socket, QStringLiteral("backend-not-ready"), tr("The requested service backend is unavailable"), jobId);
        return;
    }
    const ModelCategory category = (generate || edit) ? ModelCategory::ImageGeneration : ModelCategory::Upscaler;
    const bool routed = options.contains("presetId") || options.contains("qualityId");
    std::optional<KiraService::GenerationPreset> preset;
    std::optional<KiraService::ModelQuality> quality;
    PromptFlowRun flow;
    if (routed) {
        if ((!generate && !edit) || !options.value("qualityId").isString() || options.value("qualityId").toString().isEmpty()
            || (generate && (!options.value("presetId").isString() || options.value("presetId").toString().isEmpty()))
            || (edit && options.contains("presetId")) || !options.value("styleId").toString().isEmpty()
            || (options.contains("styleId") && !options.value("styleId").isString())
            || (options.contains("modelId") && !options.value("modelId").isString())) {
            sendError(socket, "invalid-request", tr("Invalid preset/quality selection"), jobId); return;
        }
        QString error;
        const auto routing = m_models->routing(&error);
        if (!routing) { sendError(socket, "model-routing-unavailable", error, jobId); return; }
        QList<KiraService::ModelQuality> qualities = routing->editingQualities;
        if (generate) {
            for (const auto &p : routing->presets)
                if (p.id == options.value("presetId").toString()) { preset = p; break; }
            if (!preset) { sendError(socket, "model-routing-unavailable", tr("The preset is unavailable"), jobId); return; }
            qualities = preset->qualities;
        }
        QString modelId;
        for (const auto &q : qualities)
            if (q.id == options.value("qualityId").toString()) { modelId = q.modelId; quality = q; break; }
        if (modelId.isEmpty()) { sendError(socket, "model-routing-unavailable", tr("The quality has no assigned model"), jobId); return; }
        const auto supplied = options.value("modelId").toString();
        if (!supplied.isEmpty() && supplied != modelId) {
            sendError(socket, "invalid-request", tr("The supplied model conflicts with the quality assignment"), jobId); return;
        }
        options.insert("modelId", modelId);
        if (preset) {
            if (options.contains("skipPromptFlow") && !options.value("skipPromptFlow").isBool()) {
                sendError(socket, "invalid-request", tr("Invalid prompt flow option"), jobId); return;
            }
            flow.routed = true;
            if (!options.take("skipPromptFlow").toBool() && !preparePromptFlow(socket, jobId, *preset, flow)) return;
        }
    }
    const std::optional<ModelInfo> model = m_models->model(category, options.value(QStringLiteral("modelId")).toString());
    if (!model.has_value()) { sendError(socket, QStringLiteral("model-unavailable"), tr("The requested model is unavailable"), jobId); return; }
    if ((generate && !model->recommendedOptions.value("canGenerate").toBool(true))
        || (edit && !model->recommendedOptions.value("canEdit").toBool(true))) {
        sendError(socket, "model-unavailable", tr("The model does not support this image operation"), jobId);
        return;
    }
    if (upscale && options.contains(QStringLiteral("scale"))) {
        sendError(socket, QStringLiteral("invalid-request"), tr("Upscale is fixed at 2×"), jobId);
        return;
    }
    if (m_queue.size() >= MaximumQueuedJobs) { sendError(socket, QStringLiteral("queue-full"), tr("The service queue is full"), jobId); return; }
    if ((generate || edit) && (!validGenerationOptions(options)
        || (edit && options.value("prompt").toString().trimmed().isEmpty()))) {
        sendError(socket, QStringLiteral("invalid-request"), tr("Invalid generation options"), jobId);
        return;
    }
    if (!edit && options.contains("inputCount") && (!generate || options.value("hasInput").toBool())) {
        sendError(socket, QStringLiteral("invalid-request"),
            tr("Reference images are only supported for image editing and generation without a base image"), jobId);
        return;
    }
    if (quality) {
        if (!options.contains("steps") && quality->steps) options.insert("steps", *quality->steps);
        if (!options.contains("cfgScale") && quality->cfgScale) options.insert("cfgScale", *quality->cfgScale);
    }
    if (options.contains("styleId") && (!options.value("styleId").isString()
        || (!generate && !options.value("styleId").toString().isEmpty()))) {
        sendError(socket, "invalid-request", tr("Styles are only supported for image generation"), jobId);
        return;
    }
    const QString styleId = options.value("styleId").toString();
    if (preset) {
        flow.prompt = options.value("prompt").toString().trimmed();
        flow.negativePrompt = options.value("negativePrompt").toString().trimmed();
        flow.positivePrefix = preset->positivePrefix;
        flow.negativePrefix = preset->negativePrefix;
        // With a prompt flow, prefixes are composed after its last step instead.
        if (!flow.active()) {
            options.insert("prompt", KiraService::Protocol::composeStyledPrompt(preset->positivePrefix, flow.prompt));
            options.insert("negativePrompt", KiraService::Protocol::composeStyledPrompt(preset->negativePrefix, flow.negativePrompt));
        }
    }
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
    if (m_pendingUploads.contains(socket)) {
        sendError(socket, "upload-in-progress", tr("Finish the pending image upload before submitting another job"), jobId);
        return;
    }
    const bool references = options.contains("inputCount");
    const bool needsInput = edit || upscale || references || options.value(QStringLiteral("hasInput")).toBool();
    auto submission = std::make_shared<Submission>();
    submission->id = jobId;
    submission->operation = edit ? Operation::Edit : generate ? Operation::Generate : Operation::Upscale;
    submission->options = options;
    submission->flow = flow;
    submission->expectedInputs = options.value("inputCount").toInt(1);
    submission->modelPath = model->filePath;
    if ((generate || edit) && model->companionRole == QStringLiteral("vae") && !model->companionFileName.isEmpty())
        submission->vaePath = QDir(QFileInfo(model->filePath).absolutePath()).filePath(model->companionFileName);
    const QDir modelDirectory(QFileInfo(model->filePath).absolutePath());
    const auto filePath = [&](const QString &role) {
        const auto name = model->companionFiles.value(role).toString();
        return name.isEmpty() ? QString() : modelDirectory.filePath(name);
    };
    if (generate || edit) {
        if (model->companionFiles.contains("vae")) submission->vaePath = filePath("vae");
        submission->llmPath = filePath("llm");
        for (const auto &key : {QString("vaeModelId"), QString("textEncoderModelId")}) {
            const auto id = model->recommendedOptions.value(key).toString();
            if (id.isEmpty()) continue;
            const auto target = m_models->modelById(id);
            const bool valid = target && (key == "vaeModelId"
                ? id.startsWith("companion:") && target->recommendedOptions.value("subtype") == "vae"
                : id.startsWith("llm-chat:") && target->recommendedOptions.value("canBeTextEncoder").toBool());
            if (!valid) {
                sendError(socket, "model-unavailable", tr("An assigned VAE or text encoder is unavailable"), jobId);
                return;
            }
            if (key == "vaeModelId") submission->vaePath = target->filePath;
            else {
                submission->llmPath = target->filePath;
                if (!target->mmprojModelId.isEmpty()) {
                    const auto projector = m_models->modelById(target->mmprojModelId);
                    if (!projector || !target->mmprojModelId.startsWith("companion:")
                        || projector->recommendedOptions.value("subtype") != "mmproj") {
                        sendError(socket, "model-unavailable", tr("The text encoder's MMPROJ is unavailable"), jobId);
                        return;
                    }
                    submission->llmVisionPath = projector->filePath;
                }
            }
        }
        submission->diffusionModel = model->modelType == "diffusion";
    }
    submission->client = socket;
    if (needsInput) {
        m_pendingUploads.insert(socket, submission);
        send(socket, {{QStringLiteral("type"), QStringLiteral("upload-required")}, {QStringLiteral("jobId"), jobId},
            {"inputRole", generate && references ? "reference" : "source"}});
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = jobId;
    job->operation = submission->operation;
    job->options = options;
    job->flow = flow;
    job->modelPath = model->filePath;
    job->vaePath = submission->vaePath;
    job->llmPath = submission->llmPath;
    job->llmVisionPath = submission->llmVisionPath;
    job->clipLPath = submission->clipLPath;
    job->clipGPath = submission->clipGPath;
    job->diffusionModel = submission->diffusionModel;
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
            if (category == ModelCategory::ImageGeneration || category == ModelCategory::LlmChat)
                modelObject.insert("explicitContent", model.explicitContent);
            modelObject.insert("displayName", model.displayName);
            if (category == ModelCategory::LlmChat)
                modelObject.insert("mmprojModelId", model.mmprojModelId);
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
    if (message.contains("mmprojModelId")) {
        if (!message.value("mmprojModelId").isString()) {
            send(socket, {{"type", "error"}, {"code", "invalid-model-install"}, {"installId", installId},
                {"message", tr("MMPROJ model ID must be a string")}}); return;
        }
        request.mmprojModelId = message.value("mmprojModelId").toString();
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
    if (message.contains("mmprojModelId") && !message.value("mmprojModelId").isString()) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-configure"}, {"id", id},
            {"message", tr("MMPROJ model ID must be a string")}}); return;
    }
    const std::optional<QString> mmprojModelId = message.contains("mmprojModelId")
        ? std::optional<QString>(message.value("mmprojModelId").toString()) : std::nullopt;
    if (!message.value("recommendedOptions").isObject() || !m_models->configure(id, message.value("recommendedOptions").toObject(), &error, explicitContent, displayName, mmprojModelId)) {
        send(socket, {{"type", "error"}, {"code", "invalid-model-configure"}, {"id", id}, {"message", error.isEmpty() ? tr("Recommended options must be an object") : error}});
        return;
    }
    const auto model = *m_models->modelById(id);
    QJsonObject value{{"id", model.id}, {"name", model.name}, {"fileName", model.fileName}, {"bytes", model.fileSize}, {"recommendedOptions", model.recommendedOptions}};
    if (model.id.startsWith("image-generation:") || model.id.startsWith("llm-chat:"))
        value.insert("explicitContent", model.explicitContent);
    value.insert("displayName", model.displayName);
    value.insert("mmprojModelId", model.mmprojModelId);
    value.insert("companionFiles", model.companionFiles);
    value.insert("modelType", model.modelType);
    if (!model.companionFileName.isEmpty()) {
        value.insert("companionFileName", model.companionFileName);
        value.insert("companionRole", model.companionRole);
    }
    send(socket, {{"type", "models.configure.completed"}, {"id", id}, {"model", value}});
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
        return job && (job->modelPath == path || job->vaePath == path || job->llmPath == path || job->llmVisionPath == path
            || std::any_of(job->flow.steps.cbegin() + job->flow.index, job->flow.steps.cend(),
                           [&path](const PromptFlowJobStep &step) { return step.modelPath == path; }));
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

void KiraServiceDaemon::processBinaryMessage(QWebSocket* socket, const QByteArray& data)
{
    const auto submission = m_pendingUploads.value(socket);
    if (!submission) { sendError(socket, "unexpected-upload", tr("No image upload is pending")); return; }
    if (data.isEmpty() || data.size() > m_configuration.maximumImageBytes) {
        m_pendingUploads.remove(socket);
        sendError(socket, "invalid-image", tr("The image upload is empty or too large"), submission->id);
        return;
    }
    submission->inputs.append(data);
    if (submission->inputs.size() < submission->expectedInputs) {
        send(socket, {{QStringLiteral("type"), QStringLiteral("upload-required")}, {QStringLiteral("jobId"), submission->id},
            {"inputRole", "reference"}, {"inputIndex", int(submission->inputs.size())}});
        return;
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
    job->flow = submission->flow;
    job->modelPath = submission->modelPath;
    job->vaePath = submission->vaePath;
    job->llmPath = submission->llmPath;
    job->llmVisionPath = submission->llmVisionPath;
    job->clipLPath = submission->clipLPath;
    job->clipGPath = submission->clipGPath;
    job->diffusionModel = submission->diffusionModel;
    job->client = socket;
    for (int index = 0; index < submission->inputs.size(); ++index) {
        const QByteArray &bytes = submission->inputs.at(index);
        const QString path = job->directory->filePath(index == 0 ? QStringLiteral("input.png") : QStringLiteral("input-%1.png").arg(index + 1));
        QFile input(path);
        if (!job->directory->isValid() || !input.open(QIODevice::WriteOnly) || input.write(bytes) != bytes.size()) {
            sendError(socket, "storage-error", tr("Could not save image upload"), job->id);
            return;
        }
        input.close();
        job->inputPaths.append(path);
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
    runActiveJob();
    broadcastQueuePositions();
}

void KiraServiceDaemon::runActiveJob()
{
    if (m_activeJob->operation == Operation::PromptEnhance) {
        startPromptEnhancement();
        return;
    }
    if (m_activeJob->flow.active()) {
        startPromptFlowStep();
        return;
    }
    if (m_activeJob->operation == Operation::Generate || m_activeJob->operation == Operation::Edit) {
        m_activeJob->outputPath = m_activeJob->directory->filePath(QStringLiteral("result.png"));
        GenerationRequest request;
        const QJsonObject &o = m_activeJob->options;
        request.prompt = o.value(QStringLiteral("prompt")).toString(); request.negativePrompt = o.value(QStringLiteral("negativePrompt")).toString();
        request.sampler = o.value(QStringLiteral("sampler")).toString(); request.scheduler = o.value(QStringLiteral("scheduler")).toString();
        request.width = o.value(QStringLiteral("width")).toInt(request.width);
        request.height = o.value(QStringLiteral("height")).toInt(request.height);
        request.steps = o.value(QStringLiteral("steps")).toInt(request.steps);
        request.cfgScale = o.value(QStringLiteral("cfgScale")).toDouble(7.0); request.clipSkip = o.value(QStringLiteral("clipSkip")).toInt(-1); request.seed = o.value(QStringLiteral("seed")).toInt(-1);
        request.baseImageStrength = o.value(QStringLiteral("baseImageStrength")).toDouble(0.55); request.outputPath = m_activeJob->outputPath;
        request.modelPath = m_activeJob->modelPath;
        request.vaePath = m_activeJob->vaePath;
        request.llmPath = m_activeJob->llmPath;
        request.llmVisionPath = m_activeJob->llmVisionPath;
        request.clipLPath = m_activeJob->clipLPath;
        request.clipGPath = m_activeJob->clipGPath;
        request.diffusionModel = m_activeJob->diffusionModel;
        if (m_activeJob->operation == Operation::Edit || o.contains(QStringLiteral("inputCount")))
            request.referenceImagePaths = m_activeJob->inputPaths;
        else if (o.value(QStringLiteral("hasInput")).toBool()) request.baseImagePath = m_activeJob->inputPaths.value(0);
        if (!m_sdRunner || !m_sdRunner->runGeneration(request)) { sendError(m_activeJob->client, QStringLiteral("start-failed"), tr("Could not start generation"), m_activeJob->id); m_activeJob.reset(); startNextJob(); }
    } else if (m_activeJob->operation == Operation::Upscale) {
        m_activeJob->outputPath = m_activeJob->directory->filePath(QStringLiteral("result.png"));
        const QStringList arguments = {QStringLiteral("--mode"), QStringLiteral("upscale"), QStringLiteral("--init-img"), m_activeJob->inputPaths.value(0), QStringLiteral("--upscale-model"), m_activeJob->modelPath, QStringLiteral("--upscale-repeats"), QStringLiteral("1"), QStringLiteral("--upscale-tile-size"), QStringLiteral("128"), QStringLiteral("--output"), m_activeJob->outputPath};
        if (!m_sdRunner || !m_sdRunner->runUpscale(arguments, m_activeJob->outputPath)) { sendError(m_activeJob->client, QStringLiteral("start-failed"), tr("Could not start upscaling"), m_activeJob->id); m_activeJob.reset(); startNextJob(); }
    }
}

void KiraServiceDaemon::startPromptFlowStep()
{
    const auto job = m_activeJob;
    const auto &step = job->flow.steps.at(job->flow.index);
    KiraService::GenerationProgress progress;
    progress.stage = KiraService::GenerationProgressStage::EnhancingPrompt;
    progress.statusText = step.name;
    progress.flowStep = job->flow.index + 1;
    progress.flowSteps = int(job->flow.steps.size());
    progress.flowStepName = step.name;
    progress.flowStepNameFr = step.nameFr;
    send(job->client, {{"type", "progress"}, {"jobId", job->id}, {"progress", KiraService::Protocol::progressToJson(progress)}});
    const QString instructions = step.instructions.trimmed().isEmpty()
        ? QStringLiteral("Improve the image-generation prompts.") : step.instructions.trimmed();
    const QString system = instructions + QStringLiteral(
        "\n\nThe input after these instructions is a JSON object {\"positive\": string, \"negative\": string} holding "
        "the positive and negative image-generation prompts. Reply with only a JSON object of the same shape, without explanations, "
        "headings or code fences.");
    const QString input = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"positive", job->flow.prompt}, {"negative", job->flow.negativePrompt}}).toJson(QJsonDocument::Compact));
    if (!startLlm(step.modelPath, step.parameters, system, input, false)) {
        sendError(job->client, "storage-error", tr("Could not prepare prompt flow step “%1”").arg(step.name), job->id);
        m_activeJob.reset(); startNextJob();
    }
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
    else {
        QFile output(job->outputPath);
        const bool opened = output.open(QIODevice::ReadOnly);
        const QByteArray bytes = opened ? output.readAll() : QByteArray();
        if (bytes.isEmpty()) sendError(job->client, QStringLiteral("result-read-failed"), tr("Could not read generated image"), job->id);
        else {
            QJsonObject result{{QStringLiteral("type"), QStringLiteral("result")}, {QStringLiteral("jobId"), job->id},
                {QStringLiteral("operation"), job->operation == Operation::Edit ? QStringLiteral("image.edit") : QStringLiteral("image.generate")},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")}};
            if (job->flow.routed) {
                result.insert("prompt", job->flow.prompt);
                result.insert("negativePrompt", job->flow.negativePrompt);
            }
            send(job->client, result);
            if (job->client) job->client->sendBinaryMessage(bytes);
        }
    }
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
        if (m_activeJob->operation == Operation::PromptEnhance || m_activeJob->flow.active()) m_promptProcess.kill();
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
    const QString state = isImageReady() || isPromptEnhancementReady() ? QStringLiteral("ready") : QStringLiteral("unavailable");
    QJsonArray operations;
    operations.append("model-routing");
    if (isImageReady()) {
        operations.append(QStringLiteral("image.generate"));
        operations.append(QStringLiteral("image.upscale"));
        operations.append(QStringLiteral("image.edit"));
    }
    if (isPromptEnhancementReady()) {
        operations.append("prompt.enhance");
        operations.append("prompt-flow.run");
    }
    send(socket, {{QStringLiteral("type"), QStringLiteral("capabilities")}, {QStringLiteral("state"), state}, {QStringLiteral("operations"), operations}});
}

void KiraServiceDaemon::modelRouting(QWebSocket *socket, const QJsonObject &message)
{
    const auto operation = message.value("type").toString();
    QString error;
    auto routing = m_models->routing(&error);
    if (routing && operation == "model-routing.configure") {
        routing = KiraService::Protocol::modelRoutingFromJson(message.value("routing").toObject(), &error);
        if (routing && !m_models->configureRouting(*routing, &error)) routing.reset();
        else if (routing) routing = m_models->routing(&error);
    }
    if (!routing) {
        send(socket, {{"type", "error"}, {"code", "model-routing-invalid"}, {"operation", operation}, {"message", error}});
        return;
    }
    send(socket, {{"type", operation + ".completed"}, {"routing", KiraService::Protocol::modelRoutingToJson(*routing)}});
    if (operation == "model-routing.configure")
        for (auto it = m_authenticatedClients.cbegin(); it != m_authenticatedClients.cend(); ++it)
            send(it.key(), {{"type", "model-routing.changed"}});
}

bool KiraServiceDaemon::preparePromptFlow(QWebSocket *socket, const QString &jobId,
                                          const KiraService::GenerationPreset &preset, PromptFlowRun &flow) const
{
    if (!preset.promptFlow.isEmpty() && !isPromptEnhancementReady()) {
        sendError(socket, "backend-not-ready", tr("llama-completion is unavailable for the prompt flow"), jobId);
        return false;
    }
    for (const auto &step : preset.promptFlow) {
        if (step.modelId.isEmpty()) {
            sendError(socket, "model-routing-unavailable", tr("Prompt flow step “%1” has no assigned LLM").arg(step.name), jobId);
            return false;
        }
        const auto llm = m_models->model(ModelCategory::LlmChat, step.modelId);
        if (!llm) {
            sendError(socket, "model-routing-unavailable", tr("The LLM for prompt flow step “%1” is unavailable").arg(step.name), jobId);
            return false;
        }
        if (QFileInfo(llm->filePath).suffix().toLower() != "gguf") {
            sendError(socket, "llm-format-unsupported", tr("llama-completion requires a compatible GGUF model"), jobId);
            return false;
        }
        flow.steps.append({step.name, step.nameFr, llm->filePath,
            llm->recommendedOptions.isEmpty() ? KiraService::Protocol::defaultModelOptions("llm-chat") : llm->recommendedOptions,
            step.instructions});
    }
    return true;
}

void KiraServiceDaemon::submitPromptFlow(QWebSocket *socket, const QJsonObject &message)
{
    const auto id = message.value("jobId").toString();
    const auto options = message.value("options").toObject();
    for (const auto &key : options.keys())
        if (key != "presetId" && key != "prompt" && key != "negativePrompt") {
            sendError(socket, "invalid-request", tr("Unsupported prompt flow field"), id); return;
        }
    if (!message.value("options").isObject() || !options.value("presetId").isString() || !options.value("prompt").isString()
        || options.value("prompt").toString().toUtf8().size() > 65536
        || (options.contains("negativePrompt") && (!options.value("negativePrompt").isString()
                                                   || options.value("negativePrompt").toString().toUtf8().size() > 65536))) {
        sendError(socket, "invalid-request", tr("A prompt flow requires a preset and prompts of at most 64 KiB"), id); return;
    }
    QString error;
    const auto routing = m_models->routing(&error);
    if (!routing) { sendError(socket, "model-routing-unavailable", error, id); return; }
    const auto preset = std::find_if(routing->presets.cbegin(), routing->presets.cend(),
        [&](const KiraService::GenerationPreset &p) { return p.id == options.value("presetId").toString(); });
    if (preset == routing->presets.cend()) { sendError(socket, "model-routing-unavailable", tr("The preset is unavailable"), id); return; }
    if (preset->promptFlow.isEmpty()) { sendError(socket, "prompt-flow-empty", tr("The preset has no prompt flow"), id); return; }
    auto job = std::make_shared<Job>();
    if (!preparePromptFlow(socket, id, *preset, job->flow)) return;
    if (m_pendingUploads.contains(socket) || m_queue.size() >= MaximumQueuedJobs) {
        sendError(socket, "queue-full", tr("An upload is pending or the service queue is full"), id); return;
    }
    job->id = id;
    job->operation = Operation::PromptFlow;
    job->client = socket;
    job->flow.prompt = options.value("prompt").toString().trimmed();
    job->flow.negativePrompt = options.value("negativePrompt").toString().trimmed();
    enqueue(job);
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
    const auto model = m_models->model(ModelCategory::LlmChat, request.value("modelId").toString());
    if (!model) { sendError(socket, "model-unavailable", tr("The requested LLM model is unavailable"), id); return; }
    auto parameters = model->recommendedOptions.isEmpty()
        ? KiraService::Protocol::defaultModelOptions("llm-chat") : model->recommendedOptions;
    const auto overrides = request.value("parameters").toObject();
    for (const auto& key : overrides.keys()) {
        if (key == "systemPrompt") {
            if (!overrides.value(key).isString()) { sendError(socket, "invalid-request", tr("System prompt must be a string"), id); return; }
            continue;
        }
        if (!parameters.contains(key)) {
            sendError(socket, "invalid-request", tr("Unsupported LLM parameter"), id); return;
        }
        parameters.insert(key, overrides.value(key));
    }
    if (!KiraService::Protocol::validateModelOptions("llm-chat", parameters)
        || overrides.value("systemPrompt").toString().toUtf8().size() > 65536) {
        sendError(socket, "invalid-request", tr("Invalid LLM parameters"), id); return;
    }
    if (QFileInfo(model->filePath).suffix().toLower() != "gguf") {
        sendError(socket, "llm-format-unsupported", tr("llama-completion requires a compatible GGUF model"), id); return;
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
    job->options.insert("systemPrompt", overrides.value("systemPrompt").toString());
    job->options.insert("prompt", request.value("prompt"));
    enqueue(job);
}

void KiraServiceDaemon::startPromptEnhancement()
{
    const auto job = m_activeJob;
    const auto parameters = job->options;
    QString system = parameters.value("systemPrompt").toString();
    if (system.trimmed().isEmpty())
        system = QStringLiteral("Enhance the image-generation prompt that follows these instructions. Return only the enhanced prompt, without explanations, headings, quotation marks or reasoning.");
    const auto input = parameters.value("prompt").toString();
    if (!startLlm(job->modelPath, parameters, system, input.trimmed().isEmpty()
        ? QStringLiteral("Generate a new description according to the instructions above.") : input)) {
        sendError(job->client, "storage-error", tr("Could not prepare prompt enhancement"), job->id);
        m_activeJob.reset(); startNextJob();
    }
}

bool KiraServiceDaemon::startLlm(const QString &modelPath, const QJsonObject &parameters, const QString &system,
                                 const QString &input, bool penalizeRepeats)
{
    const auto job = m_activeJob;
    const QString promptPath = job->directory->filePath("prompt.txt");
    // llama-completion formats a system prompt on its own before the user turn, and chat
    // templates that require a user query (Qwen3.5 and similar) abort on it. Send the
    // instructions at the start of the single user message instead.
    const QString message = system.trimmed().isEmpty() ? input : system.trimmed() + QStringLiteral("\n\n") + input;
    QFile prompt(promptPath);
    const auto bytes = message.toUtf8();
    if (!job->directory->isValid() || !prompt.open(QIODevice::WriteOnly) || prompt.write(bytes) != bytes.size())
        return false;
    prompt.close();
    QStringList arguments{"--model", modelPath, "--file", promptPath, "--single-turn", "--conversation", "--jinja", "--no-display-prompt", "--simple-io", "--color", "off",
        "--reasoning", "off", "--no-context-shift", "--ctx-size", QString::number(parameters.value("contextLength").toInt()),
        "--n-predict", QString::number(parameters.value("maxTokens").toInt()),
        // llama.cpp also penalizes prompt tokens. Flow steps repeat their instructions'
        // words and JSON keys by design, so a penalty there garbles small models.
        "--repeat-penalty", penalizeRepeats ? QStringLiteral("1.1") : QStringLiteral("1.0"),
        "--repeat-last-n", penalizeRepeats ? QString::number(parameters.value("contextLength").toInt()) : QStringLiteral("0"),
        "--samplers", "penalties;top_k;top_p;temperature", "--temp",
        QString::number(parameters.value("temperature").toDouble(), 'g', 17),
        "--top-p", QString::number(parameters.value("topP").toDouble(), 'g', 17),
        "--top-k", QStringLiteral("20")};
    m_promptOutput.clear(); m_promptError.clear();
    m_promptTimedOut = false; m_promptOutputExceeded = false;
    m_promptTimeout.start();
    m_promptProcess.start(m_configuration.llamaCompletion, arguments);
    return true;
}

void KiraServiceDaemon::handlePromptEnhancementFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_activeJob || (m_activeJob->operation != Operation::PromptEnhance && !m_activeJob->flow.active())) return;
    const bool flowStep = m_activeJob->operation != Operation::PromptEnhance;
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
        if (flowStep)
            sendError(job->client, "prompt-flow-failed", tr("Prompt flow step “%1” failed: %2")
                .arg(job->flow.steps.at(job->flow.index).name, detail), job->id);
        else sendError(job->client, code, detail, job->id);
    } else if (flowStep) {
        auto &flow = job->flow;
        const auto result = parsePromptFlowOutput(prompt, flow.negativePrompt);
        if (!result) {
            sendError(job->client, "prompt-flow-failed", tr("Prompt flow step “%1” returned an invalid result:\n%2")
                .arg(flow.steps.at(flow.index).name, prompt.left(1024)), job->id);
        } else {
            flow.prompt = result->first;
            flow.negativePrompt = result->second;
            ++flow.index;
            if (!flow.active() && job->operation == Operation::PromptFlow) {
                // A flow preview ends here and returns the final prompts without prefixes.
                send(job->client, {{"type", "result"}, {"jobId", job->id}, {"operation", "prompt-flow.run"},
                                   {"prompt", flow.prompt}, {"negativePrompt", flow.negativePrompt}});
                m_promptOutput.clear(); m_promptError.clear();
                m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
                return;
            }
            if (!flow.active()) {
                job->options.insert("prompt", KiraService::Protocol::composeStyledPrompt(flow.positivePrefix, flow.prompt));
                job->options.insert("negativePrompt", KiraService::Protocol::composeStyledPrompt(flow.negativePrefix, flow.negativePrompt));
            }
            m_promptOutput.clear(); m_promptError.clear();
            runActiveJob();
            return;
        }
    } else send(job->client, {{"type", "result"}, {"jobId", job->id}, {"operation", "prompt.enhance"}, {"prompt", prompt}});
    m_promptOutput.clear(); m_promptError.clear();
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
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
