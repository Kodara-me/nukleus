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
#include <QRegularExpression>
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

constexpr int MaximumChatMessages = 1024;
constexpr int MaximumChatBytes = 1024 * 1024;
constexpr int MaximumChatStops = 4;
constexpr int MaximumChatStopBytes = 256;
constexpr int MaximumChatSchemaBytes = 64 * 1024;

// Validates text.chat messages: an optional leading system message, then user/assistant turns ending with the user.
bool validChatMessages(const QJsonValue &value)
{
    if (!value.isArray()) return false;
    const auto messages = value.toArray();
    if (messages.isEmpty() || messages.size() > MaximumChatMessages
        || QJsonDocument(messages).toJson(QJsonDocument::Compact).size() > MaximumChatBytes) return false;
    for (int i = 0; i < messages.size(); ++i) {
        const auto message = messages.at(i).toObject();
        const auto role = message.value("role").toString();
        if (!messages.at(i).isObject() || message.size() != 2 || !message.value("role").isString()
            || !message.value("content").isString()) return false;
        if (role != "user" && role != "assistant" && (role != "system" || i != 0)) return false;
        if (role != "assistant" && message.value("content").toString().trimmed().isEmpty()) return false;
    }
    return messages.last().toObject().value("role").toString() == "user";
}

// Validates the text.chat-only overrides; sampling values are checked with the model options.
bool validChatOverrides(const QJsonObject &overrides)
{
    const QStringList allowed{"temperature", "topP", "contextLength", "maxTokens", "penalizeRepeats", "stop",
        "topK", "minP", "repeatPenalty", "seed", "engine", "gpuLayers", "kvCacheLocation", "kvCacheType",
        "flashAttention", "jsonSchema", "reasoning", "dryMultiplier", "dryBase", "dryAllowedLength",
        "dryPenaltyLastN", "xtcProbability", "xtcThreshold", "chatTemplate"};
    for (const auto &key : overrides.keys())
        if (!allowed.contains(key)) return false;
    const auto choice = [&overrides](const QString &key, const QStringList &choices) {
        return !overrides.contains(key) || choices.contains(overrides.value(key).toString());
    };
    if (overrides.contains("stop")) {
        if (!overrides.value("stop").isArray() || overrides.value("stop").toArray().size() > MaximumChatStops) return false;
        for (const auto &stop : overrides.value("stop").toArray())
            if (!stop.isString() || stop.toString().isEmpty() || stop.toString().toUtf8().size() > MaximumChatStopBytes) return false;
    }
    // A constrained reply is one JSON document; cutting it at a stop string would break it.
    if (overrides.contains("jsonSchema")
        && (!overrides.value("jsonSchema").isObject() || overrides.contains("stop")
            || QJsonDocument(overrides.value("jsonSchema").toObject()).toJson(QJsonDocument::Compact).size() > MaximumChatSchemaBytes))
        return false;
    const auto layers = overrides.value("gpuLayers");
    return (!overrides.contains("penalizeRepeats") || overrides.value("penalizeRepeats").isBool())
        && validNumber(overrides, "topK", 0, 1000, true, false)
        && validNumber(overrides, "minP", 0, 1, false, false)
        && validNumber(overrides, "repeatPenalty", 1, 2, false, false)
        && validNumber(overrides, "seed", -1, std::numeric_limits<int>::max(), true, false)
        && choice("flashAttention", {"auto", "on", "off"})
        && choice("reasoning", {"off", "on"})
        && validNumber(overrides, "dryMultiplier", 0, 10, false, false)
        && validNumber(overrides, "dryBase", 1, 10, false, false)
        && validNumber(overrides, "dryAllowedLength", 0, 100, true, false)
        && validNumber(overrides, "dryPenaltyLastN", -1, 131072, true, false)
        && validNumber(overrides, "xtcProbability", 0, 1, false, false)
        && validNumber(overrides, "xtcThreshold", 0, 1, false, false)
        // The name is checked against llama.cpp's built-in list once the engine is known.
        && (!overrides.contains("chatTemplate") || overrides.value("chatTemplate").isString())
        && choice("engine", {"server", "completion"})
        && (!overrides.contains("gpuLayers") || layers.toString() == "all" || layers.toString() == "auto"
            || validNumber(overrides, "gpuLayers", 0, 999, true))
        && choice("kvCacheLocation", {"gpu", "ram"})
        && choice("kvCacheType", {"f16", "q8_0", "q4_0"});
}

// llama.cpp load arguments shared by both chat engines: model layers, KV cache and attention kernel.
QStringList chatLaunchArguments(const QJsonObject &options)
{
    const auto layers = options.value("gpuLayers");
    QStringList arguments{"--n-gpu-layers", layers.isString() ? layers.toString() : QString::number(layers.toInt())};
    // auto fills the GPU and runs the remaining layers on the CPU; otherwise a model that does not fit
    // must fail to load instead of silently spilling onto the CPU.
    arguments += {"--fit", layers.toString() == "auto" ? "on" : "off"};
    if (options.value("kvCacheLocation").toString() == "ram") arguments += "--no-kv-offload";
    const auto cacheType = options.value("kvCacheType").toString();
    arguments += {"--cache-type-k", cacheType, "--cache-type-v", cacheType};
    arguments += {"--flash-attn", options.value("flashAttention").toString()};
    if (options.contains("chatTemplate")) arguments += {"--chat-template", options.value("chatTemplate").toString()};
    return arguments;
}

// Sampler order of prompt.enhance; chat adds DRY, min-p and XTC in llama.cpp's default positions.
QString samplerChain(const QJsonObject &options)
{
    return options.contains("minP") ? QStringLiteral("penalties;dry;top_k;top_p;min_p;xtc;temperature")
                                    : QStringLiteral("penalties;top_k;top_p;temperature");
}

// Parses the built-in chat template names from llama.cpp's --help. llama.cpp accepts any other
// string as a Jinja template source, so an unchecked typo would silently become the prompt format.
QStringList parseBuiltInChatTemplates(const QString &help)
{
    const auto start = help.indexOf("list of built-in templates:");
    if (start < 0) return {};
    const auto end = help.indexOf("(env:", start);
    const auto block = help.mid(start + 27, end < 0 ? -1 : end - start - 27);
    QStringList names;
    for (const auto &name : block.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts)) names.append(name);
    return names;
}

// llama.cpp penalizes prompt tokens too, which pushes faithful rewrites toward synonyms or another
// language; such jobs turn the penalty off entirely.
double repeatPenalty(const QJsonObject &options, bool penalizeRepeats)
{
    return penalizeRepeats ? options.value("repeatPenalty").toDouble(1.1) : 1.0;
}

int repeatLastN(const QJsonObject &options, bool penalizeRepeats)
{
    return penalizeRepeats ? options.value("contextLength").toInt() : 0;
}

// Body of an OpenAI-compatible llama-server chat completion, mirroring the completion engine's sampling.
QJsonObject chatCompletionBody(const QJsonObject &options)
{
    const bool penalize = options.value("penalizeRepeats").toBool(true);
    QJsonObject body{{"messages", options.value("messages")},
        {"temperature", options.value("temperature")}, {"top_p", options.value("topP")},
        {"top_k", options.value("topK")}, {"min_p", options.value("minP")},
        {"repeat_penalty", repeatPenalty(options, penalize)}, {"repeat_last_n", repeatLastN(options, penalize)},
        {"seed", options.value("seed")}, {"max_tokens", options.value("maxTokens")},
        {"dry_multiplier", options.value("dryMultiplier")}, {"dry_base", options.value("dryBase")},
        {"dry_allowed_length", options.value("dryAllowedLength")}, {"dry_penalty_last_n", options.value("dryPenaltyLastN")},
        {"xtc_probability", options.value("xtcProbability")}, {"xtc_threshold", options.value("xtcThreshold")},
        {"samplers", QJsonArray::fromStringList(samplerChain(options).split(QLatin1Char(';')))},
        {"cache_prompt", true}, {"stream", false}};
    if (!options.value("stop").toArray().isEmpty()) body.insert("stop", options.value("stop"));
    if (options.value("jsonSchema").isObject())
        body.insert("response_format", QJsonObject{{"type", "json_schema"},
            {"json_schema", QJsonObject{{"schema", options.value("jsonSchema")}}}});
    return body;
}

// A schema-constrained reply must parse as JSON; a reply cut by maxTokens does not.
std::optional<QJsonValue> structuredReply(const QString &text)
{
    QJsonParseError error;
    // Wrapping lets QJsonDocument accept any JSON value, not only objects and arrays.
    const auto document = QJsonDocument::fromJson("[" + text.toUtf8() + "]", &error);
    if (error.error != QJsonParseError::NoError || document.array().size() != 1) return std::nullopt;
    return document.array().first();
}

// Removes reasoning blocks and cuts the reply at the first stop string.
QString cleanChatReply(QString text, const QJsonArray &stops)
{
    text.remove(QRegularExpression("<think>.*?</think>", QRegularExpression::DotMatchesEverythingOption));
    const auto unclosed = text.indexOf("<think>");
    if (unclosed >= 0) text.truncate(unclosed);
    for (const auto &stop : stops) {
        const auto index = text.indexOf(stop.toString());
        if (index >= 0) text.truncate(index);
    }
    return text.trimmed();
}

}

struct KiraServiceDaemon::Submission
{
    QString id;
    Operation operation = Operation::Generate;
    QJsonObject options;
    std::optional<RoutedPrompts> routedPrompts;
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
    std::optional<RoutedPrompts> routedPrompts;
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
    connect(&m_promptProcess, &QProcess::finished, this, &KiraServiceDaemon::handleLlmFinished);
    m_llamaServer = std::make_unique<LlamaServerRunner>(m_configuration.llamaServer, m_configuration.llamaServerIdleMs);
    // Queued, so a reply reported while a job is being started or canceled never re-enters the queue logic.
    connect(m_llamaServer.get(), &LlamaServerRunner::finished, this, &KiraServiceDaemon::handleLlamaServerFinished,
            Qt::QueuedConnection);
    connect(&m_promptProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_promptError = m_promptProcess.errorString().toUtf8();
            handleLlmFinished(-1, QProcess::CrashExit);
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
    const bool textChat = operationName == QStringLiteral("text.chat");
    const bool textChatLoad = operationName == QStringLiteral("text.chat.load");
    const bool textChatUnload = operationName == QStringLiteral("text.chat.unload");
    QJsonObject options = message.value(QStringLiteral("options")).toObject();
    const QString jobId = message.value(QStringLiteral("jobId")).toString();
    bool duplicateId = jobForId(jobId) != nullptr;
    for (const auto &pending : m_pendingUploads) {
        duplicateId = duplicateId || pending->id == jobId;
    }
    if ((!generate && !upscale && !edit && !promptEnhance && !textChat && !textChatLoad && !textChatUnload)
        || jobId.isEmpty() || duplicateId) { sendError(socket, QStringLiteral("invalid-request"), tr("Invalid job request"), jobId); return; }
    if (promptEnhance) { submitPromptEnhancement(socket, message); return; }
    if (textChat || textChatLoad) { submitTextChat(socket, message, textChat ? Operation::TextChat : Operation::TextChatLoad); return; }
    if (textChatUnload) { submitTextChatUnload(socket, message); return; }
    if (!isImageReady()) {
        sendError(socket, QStringLiteral("backend-not-ready"), tr("The requested service backend is unavailable"), jobId);
        return;
    }
    const ModelCategory category = (generate || edit) ? ModelCategory::ImageGeneration : ModelCategory::Upscaler;
    std::optional<KiraService::GenerationPreset> preset;
    std::optional<KiraService::EditingQuality> quality;
    std::optional<RoutedPrompts> routedPrompts;
    if (options.contains("qualityId")) {
        // Editing is routed by quality; generation is routed by preset only.
        if (!edit || !options.value("qualityId").isString() || options.value("qualityId").toString().isEmpty()
            || options.contains("presetId") || !options.value("styleId").toString().isEmpty()
            || (options.contains("modelId") && !options.value("modelId").isString())) {
            sendError(socket, "invalid-request", tr("Invalid editing quality selection"), jobId); return;
        }
        QString error;
        const auto routing = m_models->routing(&error);
        if (!routing) { sendError(socket, "model-routing-unavailable", error, jobId); return; }
        if (const auto *q = routing->editingQuality(options.value("qualityId").toString())) quality = *q;
        if (!quality) { sendError(socket, "model-routing-unavailable", tr("The editing quality is unavailable"), jobId); return; }
        if (quality->model.modelId.isEmpty()) {
            sendError(socket, "model-routing-unavailable", tr("The editing quality has no assigned model"), jobId); return;
        }
        const auto supplied = options.value("modelId").toString();
        if (!supplied.isEmpty() && supplied != quality->model.modelId) {
            sendError(socket, "invalid-request", tr("The supplied model conflicts with the editing quality assignment"), jobId); return;
        }
        options.insert("modelId", quality->model.modelId);
    }
    if (options.contains("presetId")) {
        if (!generate || !options.value("presetId").isString() || options.value("presetId").toString().isEmpty()
            || !options.value("styleId").toString().isEmpty()
            || (options.contains("styleId") && !options.value("styleId").isString())
            || (options.contains("modelId") && !options.value("modelId").isString())) {
            sendError(socket, "invalid-request", tr("Invalid preset selection"), jobId); return;
        }
        QString error;
        const auto routing = m_models->routing(&error);
        if (!routing) { sendError(socket, "model-routing-unavailable", error, jobId); return; }
        for (const auto &p : routing->presets)
            if (p.id == options.value("presetId").toString()) { preset = p; break; }
        if (!preset) { sendError(socket, "model-routing-unavailable", tr("The preset is unavailable"), jobId); return; }
        const QString modelId = preset->model.modelId;
        if (modelId.isEmpty()) { sendError(socket, "model-routing-unavailable", tr("The preset has no assigned model"), jobId); return; }
        const auto supplied = options.value("modelId").toString();
        if (!supplied.isEmpty() && supplied != modelId) {
            sendError(socket, "invalid-request", tr("The supplied model conflicts with the preset assignment"), jobId); return;
        }
        options.insert("modelId", modelId);
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
        if (!options.contains("steps") && quality->model.steps) options.insert("steps", *quality->model.steps);
        if (!options.contains("cfgScale") && quality->model.cfgScale) options.insert("cfgScale", *quality->model.cfgScale);
        // The quality's prefixes lead the validated instruction sent to sd-cli; clients never see them.
        options.insert("prompt", KiraService::Protocol::composeStyledPrompt(quality->positivePrefix, options.value("prompt").toString()));
        options.insert("negativePrompt", KiraService::Protocol::composeStyledPrompt(quality->negativePrefix, options.value("negativePrompt").toString()));
    }
    if (preset) {
        if (!options.contains("steps") && preset->model.steps) options.insert("steps", *preset->model.steps);
        if (!options.contains("cfgScale") && preset->model.cfgScale) options.insert("cfgScale", *preset->model.cfgScale);
    }
    if (options.contains("styleId") && (!options.value("styleId").isString()
        || (!generate && !options.value("styleId").toString().isEmpty()))) {
        sendError(socket, "invalid-request", tr("Styles are only supported for image generation"), jobId);
        return;
    }
    const QString styleId = options.value("styleId").toString();
    if (preset) {
        routedPrompts = RoutedPrompts{options.value("prompt").toString().trimmed(),
                                      options.value("negativePrompt").toString().trimmed()};
        options.insert("prompt", KiraService::Protocol::composeStyledPrompt(preset->positivePrefix, routedPrompts->prompt));
        options.insert("negativePrompt", KiraService::Protocol::composeStyledPrompt(preset->negativePrefix, routedPrompts->negativePrompt));
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
    submission->routedPrompts = routedPrompts;
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
    job->routedPrompts = routedPrompts;
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
    for (auto it = m_authenticatedClients.cbegin(); it != m_authenticatedClients.cend(); ++it)
        if (it.value() && it.key() != socket) send(it.key(), {{"type", "models.changed"}, {"id", id}});
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
        return job && (job->modelPath == path || job->vaePath == path || job->llmPath == path || job->llmVisionPath == path);
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
    job->routedPrompts = submission->routedPrompts;
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

bool KiraServiceDaemon::isServerChat(const Job &job) const
{
    return job.operation == Operation::TextChatLoad
        || (job.operation == Operation::TextChat && job.options.value("engine").toString() == "server");
}

void KiraServiceDaemon::runActiveJob()
{
    m_activeJobClock.start();
    const bool chatModelLoaded = m_llamaServer->isRunning();
    // A loaded chat model would compete with any other GPU work for memory.
    if (!isServerChat(*m_activeJob)) m_llamaServer->stop();
    if (m_activeJob->operation == Operation::TextChatUnload) {
        send(m_activeJob->client, {{"type", "result"}, {"jobId", m_activeJob->id}, {"operation", "text.chat.unload"},
            {"unloaded", chatModelLoaded}, {"durationMs", m_activeJobClock.elapsed()}});
        m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
        return;
    }
    if (m_activeJob->operation == Operation::PromptEnhance) {
        startPromptEnhancement();
        return;
    }
    if (m_activeJob->operation == Operation::TextChat || m_activeJob->operation == Operation::TextChatLoad) {
        startTextChat();
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
            if (job->routedPrompts) {
                result.insert("prompt", job->routedPrompts->prompt);
                result.insert("negativePrompt", job->routedPrompts->negativePrompt);
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
        if (isServerChat(*m_activeJob)) m_llamaServer->cancel();
        else if (m_activeJob->operation == Operation::PromptEnhance || m_activeJob->operation == Operation::TextChat)
            m_promptProcess.kill();
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
    if (isPromptEnhancementReady()) operations.append("prompt.enhance");
    const auto chatEngines = readyTextChatEngines();
    if (!chatEngines.isEmpty()) operations.append("text.chat");
    if (chatEngines.contains("server")) {
        operations.append("text.chat.load");
        operations.append("text.chat.unload");
    }
    send(socket, {{QStringLiteral("type"), QStringLiteral("capabilities")}, {QStringLiteral("state"), state}, {QStringLiteral("operations"), operations},
        {QStringLiteral("textChatEngines"), QJsonArray::fromStringList(chatEngines)}});
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
        if (key == "penalizeRepeats") {
            if (!overrides.value(key).isBool()) { sendError(socket, "invalid-request", tr("penalizeRepeats must be a boolean"), id); return; }
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
    // Rewrites that must reuse the input's words (translation, faithful improvement) turn it off.
    job->options.insert("penalizeRepeats", overrides.value("penalizeRepeats").toBool(true));
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
        ? QStringLiteral("Generate a new description according to the instructions above.") : input,
        parameters.value("penalizeRepeats").toBool(true))) {
        sendError(job->client, "storage-error", tr("Could not prepare prompt enhancement"), job->id);
        m_activeJob.reset(); startNextJob();
    }
}

QStringList KiraServiceDaemon::builtInChatTemplates(const QString &executable) const
{
    if (!m_chatTemplates.contains(executable)) {
        QProcess help;
        help.setProcessChannelMode(QProcess::MergedChannels);
        help.start(executable, {"--help"});
        const bool finished = help.waitForFinished(10000);
        const auto names = finished ? parseBuiltInChatTemplates(QString::fromUtf8(help.readAll())) : QStringList();
        if (!finished) help.kill();
        // An unreadable list is not cached, so a later request can retry.
        if (names.isEmpty()) return {};
        m_chatTemplates.insert(executable, names);
    }
    return m_chatTemplates.value(executable);
}

QStringList KiraServiceDaemon::readyTextChatEngines() const
{
    QStringList engines;
    if (m_llamaServer->isAvailable()) engines.append("server");
    if (isPromptEnhancementReady()) engines.append("completion");
    return engines;
}

void KiraServiceDaemon::submitTextChat(QWebSocket* socket, const QJsonObject& message, Operation operation)
{
    const auto id = message.value("jobId").toString();
    const auto request = message.value("options").toObject();
    const auto overrides = request.value("parameters").toObject();
    // text.chat.load takes the same model and parameters as text.chat, without messages, so a client can
    // preload exactly what its chat requests will use; only the load settings matter to it.
    const bool load = operation == Operation::TextChatLoad;
    bool knownKeys = true;
    for (const auto& key : request.keys())
        knownKeys = knownKeys && (key == "modelId" || key == "parameters" || (!load && key == "messages"));
    if (!message.value("options").isObject() || !knownKeys || !request.value("modelId").isString()
        || (!load && !validChatMessages(request.value("messages")))
        || (load && overrides.value("engine").toString("server") != "server")
        || (request.contains("parameters") && !request.value("parameters").isObject()) || !validChatOverrides(overrides)) {
        sendError(socket, "invalid-request", load ? tr("Invalid text chat load request") : tr("Invalid text chat request"), id);
        return;
    }
    const auto model = m_models->model(ModelCategory::LlmChat, request.value("modelId").toString());
    if (!model) { sendError(socket, "model-unavailable", tr("The requested LLM model is unavailable"), id); return; }
    auto parameters = model->recommendedOptions.isEmpty()
        ? KiraService::Protocol::defaultModelOptions("llm-chat") : model->recommendedOptions;
    for (const auto& key : {"temperature", "topP", "contextLength", "maxTokens"})
        if (overrides.contains(key)) parameters.insert(key, overrides.value(key));
    if (!KiraService::Protocol::validateModelOptions("llm-chat", parameters)) {
        sendError(socket, "invalid-request", tr("Invalid LLM parameters"), id); return;
    }
    if (QFileInfo(model->filePath).suffix().toLower() != "gguf") {
        sendError(socket, "llm-format-unsupported", tr("llama.cpp requires a compatible GGUF model"), id); return;
    }
    const auto engine = overrides.value("engine").toString("server");
    if (!readyTextChatEngines().contains(engine)) {
        sendError(socket, "backend-not-ready", tr("The %1 text chat engine is unavailable").arg(engine), id); return;
    }
    if (overrides.contains("chatTemplate")
        && !builtInChatTemplates(engine == "server" ? m_configuration.llamaServer : m_configuration.llamaCompletion)
                .contains(overrides.value("chatTemplate").toString())) {
        sendError(socket, "invalid-request", tr("Unknown llama.cpp built-in chat template"), id); return;
    }
    if (m_pendingUploads.contains(socket) || m_queue.size() >= MaximumQueuedJobs) {
        sendError(socket, "queue-full", tr("An upload is pending or the service queue is full"), id); return;
    }
    auto job = std::make_shared<Job>();
    job->id = id;
    job->operation = operation;
    job->client = socket;
    job->modelPath = model->filePath;
    job->options = parameters;
    if (!load) job->options.insert("messages", request.value("messages"));
    job->options.insert("penalizeRepeats", overrides.value("penalizeRepeats").toBool(true));
    job->options.insert("stop", overrides.value("stop").toArray());
    job->options.insert("engine", engine);
    job->options.insert("gpuLayers", overrides.contains("gpuLayers") ? overrides.value("gpuLayers") : QJsonValue("all"));
    job->options.insert("kvCacheLocation", overrides.value("kvCacheLocation").toString("gpu"));
    job->options.insert("kvCacheType", overrides.value("kvCacheType").toString("f16"));
    job->options.insert("flashAttention", overrides.value("flashAttention").toString("auto"));
    job->options.insert("topK", overrides.value("topK").toInt(20));
    job->options.insert("minP", overrides.value("minP").toDouble(0));
    job->options.insert("repeatPenalty", overrides.value("repeatPenalty").toDouble(1.1));
    job->options.insert("seed", overrides.value("seed").toInt(-1));
    if (overrides.contains("jsonSchema")) job->options.insert("jsonSchema", overrides.value("jsonSchema"));
    if (overrides.contains("chatTemplate")) job->options.insert("chatTemplate", overrides.value("chatTemplate"));
    job->options.insert("reasoning", overrides.value("reasoning").toString("off"));
    // llama.cpp's defaults: DRY and XTC stay disabled until a multiplier or probability is set.
    job->options.insert("dryMultiplier", overrides.value("dryMultiplier").toDouble(0));
    job->options.insert("dryBase", overrides.value("dryBase").toDouble(1.75));
    job->options.insert("dryAllowedLength", overrides.value("dryAllowedLength").toInt(2));
    job->options.insert("dryPenaltyLastN", overrides.value("dryPenaltyLastN").toInt(64));
    job->options.insert("xtcProbability", overrides.value("xtcProbability").toDouble(0));
    job->options.insert("xtcThreshold", overrides.value("xtcThreshold").toDouble(0.1));
    enqueue(job);
}

void KiraServiceDaemon::submitTextChatUnload(QWebSocket* socket, const QJsonObject& message)
{
    const auto id = message.value("jobId").toString();
    if ((message.contains("options") && !message.value("options").isObject()) || !message.value("options").toObject().isEmpty()) {
        sendError(socket, "invalid-request", tr("text.chat.unload takes no options"), id); return;
    }
    if (m_pendingUploads.contains(socket) || m_queue.size() >= MaximumQueuedJobs) {
        sendError(socket, "queue-full", tr("An upload is pending or the service queue is full"), id); return;
    }
    // Queued like any job, so it never interrupts a chat in progress; running it unloads the model.
    auto job = std::make_shared<Job>();
    job->id = id;
    job->operation = Operation::TextChatUnload;
    job->client = socket;
    enqueue(job);
}

void KiraServiceDaemon::startTextChat()
{
    const auto job = m_activeJob;
    if (isServerChat(*job)) {
        // Reasoning is a launch setting of llama-server; llama-completion receives it in startLlm.
        const auto arguments = chatLaunchArguments(job->options) + QStringList{"--reasoning", job->options.value("reasoning").toString()};
        // An empty body only loads the model.
        m_llamaServer->run({job->modelPath, job->options.value("contextLength").toInt(), arguments},
                           job->operation == Operation::TextChatLoad ? QJsonObject() : chatCompletionBody(job->options),
                           m_configuration.promptEnhancementTimeoutMs);
        return;
    }
    // llama-completion runs a single user turn, so the conversation is flattened into it as a transcript.
    QString system;
    QString transcript = QStringLiteral("Conversation so far:\n");
    for (const auto& value : job->options.value("messages").toArray()) {
        const auto message = value.toObject();
        const auto role = message.value("role").toString();
        if (role == "system") system = message.value("content").toString();
        else transcript += (role == "user" ? QStringLiteral("User: ") : QStringLiteral("Assistant: "))
            + message.value("content").toString() + QLatin1Char('\n');
    }
    transcript += QStringLiteral("\nWrite only the assistant's next reply, without a speaker label.");
    auto arguments = chatLaunchArguments(job->options);
    bool prepared = true;
    if (job->options.value("jsonSchema").isObject()) {
        QFile schema(job->directory->filePath("schema.json"));
        const auto bytes = QJsonDocument(job->options.value("jsonSchema").toObject()).toJson(QJsonDocument::Compact);
        prepared = schema.open(QIODevice::WriteOnly) && schema.write(bytes) == bytes.size();
        arguments += {"--json-schema-file", schema.fileName()};
    }
    if (!prepared || !startLlm(job->modelPath, job->options, system, transcript,
                               job->options.value("penalizeRepeats").toBool(true), arguments)) {
        sendError(job->client, "storage-error", tr("Could not prepare text chat"), job->id);
        m_activeJob.reset(); startNextJob();
    }
}

bool KiraServiceDaemon::startLlm(const QString &modelPath, const QJsonObject &parameters, const QString &system,
                                 const QString &input, bool penalizeRepeats, const QStringList &extraArguments)
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
    // Keep a decimal point so a whole penalty still reads as a factor (1.0, not 1).
    QString penalty = QString::number(repeatPenalty(parameters, penalizeRepeats), 'g', 15);
    if (!penalty.contains(QLatin1Char('.'))) penalty += QStringLiteral(".0");
    QStringList arguments{"--model", modelPath, "--file", promptPath, "--single-turn", "--conversation", "--jinja", "--no-display-prompt", "--simple-io", "--color", "off",
        "--reasoning", parameters.value("reasoning").toString("off"), "--no-context-shift",
        "--ctx-size", QString::number(parameters.value("contextLength").toInt()),
        "--n-predict", QString::number(parameters.value("maxTokens").toInt()),
        "--repeat-penalty", penalty,
        "--repeat-last-n", QString::number(repeatLastN(parameters, penalizeRepeats)),
        "--temp", QString::number(parameters.value("temperature").toDouble(), 'g', 17),
        "--top-p", QString::number(parameters.value("topP").toDouble(), 'g', 17)};
    // Only text.chat chooses top-k and the sampler order. prompt.enhance leaves them, and min-p, to
    // the GGUF's general.sampling recommendations or llama.cpp's defaults (top-k 40, min-p 0.05).
    if (parameters.contains("topK"))
        arguments += {"--samplers", samplerChain(parameters), "--top-k", QString::number(parameters.value("topK").toInt())};
    if (parameters.contains("minP")) arguments += {"--min-p", QString::number(parameters.value("minP").toDouble(), 'g', 15)};
    if (parameters.contains("seed")) arguments += {"--seed", QString::number(parameters.value("seed").toInt())};
    if (parameters.contains("dryMultiplier")) {
        const auto number = [&parameters](const char *key) { return QString::number(parameters.value(key).toDouble(), 'g', 15); };
        arguments += {"--dry-multiplier", number("dryMultiplier"), "--dry-base", number("dryBase"),
            "--dry-allowed-length", number("dryAllowedLength"), "--dry-penalty-last-n", number("dryPenaltyLastN"),
            "--xtc-probability", number("xtcProbability"), "--xtc-threshold", number("xtcThreshold")};
    }
    arguments += extraArguments;
    m_promptOutput.clear(); m_promptError.clear();
    m_promptTimedOut = false; m_promptOutputExceeded = false;
    m_promptTimeout.start();
    m_promptProcess.start(m_configuration.llamaCompletion, arguments);
    return true;
}

void KiraServiceDaemon::handleLlmFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_activeJob || (m_activeJob->operation != Operation::PromptEnhance && m_activeJob->operation != Operation::TextChat)) return;
    m_promptTimeout.stop();
    const auto job = m_activeJob;
    const bool chat = job->operation == Operation::TextChat;
    m_promptError = (m_promptError + m_promptProcess.readAllStandardError()).right(4096);
    const auto remaining = m_promptProcess.readAllStandardOutput();
    if (m_promptOutput.size() + remaining.size() > 1024 * 1024) m_promptOutputExceeded = true;
    else m_promptOutput += remaining;
    QString text = QString::fromUtf8(m_promptOutput).trimmed();
    const QString endMarker = QStringLiteral("[end of text]");
    if (text.endsWith(endMarker)) text.chop(endMarker.size());
    text = chat ? cleanChatReply(text, job->options.value("stop").toArray()) : text.trimmed();
    if (status != QProcess::NormalExit || exitCode != 0 || m_promptTimedOut || m_promptOutputExceeded || text.isEmpty()) {
        const QString prefix = chat ? QStringLiteral("chat-") : QStringLiteral("prompt-enhancement-");
        const QString code = prefix + (m_promptTimedOut ? "timeout" : m_promptOutputExceeded
            ? "output-too-large" : text.isEmpty() && exitCode == 0 ? "empty-result" : "failed");
        QString detail = (chat ? tr("Text chat failed (exit code %1)") : tr("Prompt enhancement failed (exit code %1)")).arg(exitCode);
        if (!m_promptError.trimmed().isEmpty()) detail += QLatin1Char('\n') + QString::fromUtf8(m_promptError).trimmed();
        sendError(job->client, code, detail, job->id);
    } else if (chat) {
        // llama-completion loads the model for every turn.
        sendChatResult(*job, text, {{"engine", "completion"}, {"coldStart", true}});
    } else send(job->client, {{"type", "result"}, {"jobId", job->id}, {"operation", "prompt.enhance"}, {"prompt", text}});
    m_promptOutput.clear(); m_promptError.clear();
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
}

void KiraServiceDaemon::handleLlamaServerFinished(const LlamaServerReply &reply)
{
    if (!m_activeJob || !isServerChat(*m_activeJob)) return;
    const auto job = m_activeJob;
    if (job->operation == Operation::TextChatLoad) {
        if (!reply.succeeded) sendError(job->client, reply.errorCode, reply.message, job->id);
        else send(job->client, {{"type", "result"}, {"jobId", job->id}, {"operation", "text.chat.load"},
            {"coldStart", reply.coldStart}, {"durationMs", m_activeJobClock.elapsed()}});
        m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
        return;
    }
    const QString text = cleanChatReply(reply.content, job->options.value("stop").toArray());
    if (!reply.succeeded) sendError(job->client, reply.errorCode, reply.message, job->id);
    else if (text.isEmpty()) sendError(job->client, "chat-empty-result", tr("The model returned an empty reply"), job->id);
    else sendChatResult(*job, text, {{"engine", "server"}, {"finishReason", reply.finishReason},
        {"usage", QJsonObject{{"promptTokens", reply.promptTokens}, {"completionTokens", reply.completionTokens}}},
        {"coldStart", reply.coldStart}});
    m_activeJob.reset(); broadcastQueuePositions(); startNextJob();
}

void KiraServiceDaemon::sendChatResult(const Job &job, const QString &text, QJsonObject result)
{
    if (job.options.value("jsonSchema").isObject()) {
        const auto structured = structuredReply(text);
        if (!structured) {
            const QString detail = result.value("finishReason").toString() == "length"
                ? tr("The reply reached maxTokens before its JSON was complete") : tr("The reply is not valid JSON");
            sendError(job.client, "chat-invalid-json", detail, job.id);
            return;
        }
        result.insert("json", *structured);
    }
    result.insert("type", "result");
    result.insert("jobId", job.id);
    result.insert("operation", "text.chat");
    result.insert("text", text);
    result.insert("durationMs", m_activeJobClock.elapsed());
    send(job.client, result);
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
