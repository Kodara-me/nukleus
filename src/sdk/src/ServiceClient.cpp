#include "nukleus/ServiceClient.h"
#include "nukleus/ServiceProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QWebSocket>
#include <algorithm>

namespace
{
Nukleus::ModelCategory categoryFromName(const QString &name)
{
    if (name == QStringLiteral("image-generation")) return Nukleus::ModelCategory::ImageGeneration;
    if (name == QStringLiteral("upscaler")) return Nukleus::ModelCategory::Upscaler;
    if (name == "companion") return Nukleus::ModelCategory::Companion;
    if (name == QStringLiteral("llm-chat")) return Nukleus::ModelCategory::LlmChat;
    if (name == QStringLiteral("animation")) return Nukleus::ModelCategory::Animation;
    return Nukleus::ModelCategory::Unknown;
}

QString categoryName(Nukleus::ModelCategory category)
{
    if (category == Nukleus::ModelCategory::ImageGeneration) return QStringLiteral("image-generation");
    if (category == Nukleus::ModelCategory::Upscaler) return QStringLiteral("upscaler");
    if (category == Nukleus::ModelCategory::Companion) return QStringLiteral("companion");
    if (category == Nukleus::ModelCategory::LlmChat) return QStringLiteral("llm-chat");
    if (category == Nukleus::ModelCategory::Animation) return QStringLiteral("animation");
    return {};
}

QString operationName(Nukleus::ImageOperation operation)
{
    if (operation == Nukleus::ImageOperation::Edit) return QStringLiteral("image.edit");
    return operation == Nukleus::ImageOperation::Generate ? QStringLiteral("image.generate") : QStringLiteral("image.upscale");
}
}

namespace Nukleus
{
ServiceClient::ServiceClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QStringLiteral("Nukleus SDK"), QWebSocketProtocol::VersionLatest, this))
{
    connect(m_socket, &QWebSocket::connected, this, &ServiceClient::authenticate);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &ServiceClient::handleTextMessage);
    connect(m_socket, &QWebSocket::binaryMessageReceived, this, &ServiceClient::handleBinaryMessage);
    connect(m_socket, &QWebSocket::disconnected, this, [this] {
        m_authenticated = false;
        updateCapabilities({});
        clearCatalog();
        failActiveInstalls(QStringLiteral("The service connection was closed."));
        failAllJobs(QStringLiteral("connection-closed"), QStringLiteral("The service connection was closed."));
        const QString code = m_connectionErrorCode.isEmpty() ? QStringLiteral("disconnected") : m_connectionErrorCode;
        const QString detail = m_connectionErrorDetail.isEmpty() ? QStringLiteral("The service connection was closed.") : m_connectionErrorDetail;
        m_connectionErrorCode.clear();
        m_connectionErrorDetail.clear();
        emit serviceStateChanged(ServiceConnectionState::Disconnected, code, detail);
    });
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        m_connectionErrorCode = QStringLiteral("connection-error");
        m_connectionErrorDetail = m_socket->errorString();
    });
}

QUrl ServiceClient::endpoint() const { return m_endpoint; }
QString ServiceClient::token() const { return m_token; }

void ServiceClient::setConfiguration(const QUrl &endpoint, const QString &token)
{
    const bool changed = m_endpoint != endpoint || m_token != token;
    m_endpoint = endpoint;
    m_token = token;
    if (changed) {
        clearCatalog();
        updateCapabilities({});
    }
}

void ServiceClient::connectToService()
{
    if (!m_endpoint.isValid() || (m_endpoint.scheme() != QStringLiteral("ws") && m_endpoint.scheme() != QStringLiteral("wss"))) {
        emit serviceStateChanged(ServiceConnectionState::Error, QStringLiteral("invalid-endpoint"), QStringLiteral("The service endpoint is invalid."));
        return;
    }
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    m_authenticated = false;
    clearCatalog();
    updateCapabilities({});
    m_connectionErrorCode.clear();
    m_connectionErrorDetail.clear();
    emit serviceStateChanged(ServiceConnectionState::Connecting, QStringLiteral("connecting"), {});
    m_socket->open(m_endpoint);
}

void ServiceClient::disconnectFromService()
{
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->close();
}

bool ServiceClient::isAuthenticated() const { return m_authenticated; }
ServiceCapabilities ServiceClient::capabilities() const { return m_capabilities; }

bool ServiceClient::supports(ImageOperation operation) const
{
    if (operation == ImageOperation::Edit) return m_capabilities.imageEdit;
    return operation == ImageOperation::Generate ? m_capabilities.imageGeneration : m_capabilities.imageUpscale;
}

bool ServiceClient::canSubmit(ImageOperation operation, const QString &modelId) const
{
    const ModelCategory category = operation == ImageOperation::Generate || operation == ImageOperation::Edit
        ? ModelCategory::ImageGeneration : ModelCategory::Upscaler;
    if (!m_authenticated || !supports(operation) || !containsModel(modelId, category)) return false;
    for (const auto& model : m_models)
        if (model.id == modelId) {
            if (operation == ImageOperation::Generate) return model.recommendedOptions.value("canGenerate").toBool(true);
            if (operation == ImageOperation::Edit) return model.recommendedOptions.value("canEdit").toBool(true);
        }
    return true;
}

bool ServiceClient::modelListAvailable() const { return m_modelListAvailable; }

QList<ModelInfo> ServiceClient::models(ModelCategory category) const
{
    if (category == ModelCategory::Unknown) return m_models;
    QList<ModelInfo> result;
    for (const ModelInfo &model : m_models) {
        if (model.category == category) result.append(model);
    }
    return result;
}

QString ServiceClient::defaultPromptEnhancerModelId() const { return defaultLlmModelId("defaultPromptEnhancer"); }

QString ServiceClient::defaultPromptTranslatorModelId() const { return defaultLlmModelId("defaultPromptTranslator"); }

QString ServiceClient::defaultImageDescriberModelId() const { return defaultLlmModelId("defaultImageDescriber"); }

QString ServiceClient::defaultLlmModelId(const QString &role) const
{
    for (const ModelInfo &model : m_models)
        if (model.category == ModelCategory::LlmChat && model.recommendedOptions.value(role).toBool()) return model.id;
    return {};
}

QList<ModelInstall> ServiceClient::installs() const { return m_installs; }

void ServiceClient::requestModels()
{
    if (m_authenticated) m_socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list\"}"));
}

QString ServiceClient::installModel(ModelCategory category, const QUrl &url, const QUrl &companionUrl, const QJsonObject &recommendedOptions, const QJsonObject &companionUrls,
    const QString &modelType, const QString &existingModelId, std::optional<bool> explicitContent, std::optional<QString> displayName, std::optional<QString> mmprojModelId)
{
    if ((existingModelId.isEmpty() && !displayName) || (displayName && (displayName->trimmed().isEmpty() || displayName->size() > 256))) return {};
    const QString categoryValue = categoryName(category);
    if (!m_authenticated || categoryValue.isEmpty() || (existingModelId.isEmpty() && (!url.isValid() || url.isEmpty()))) return {};
    const QString installId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ModelInstall install;
    install.installId = installId;
    install.category = category;
    install.url = url;
    install.companionUrl = companionUrl;
    m_installs.append(install);
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("models.install")},
                        {QStringLiteral("installId"), installId},
                        {QStringLiteral("category"), categoryValue},
                        {QStringLiteral("url"), url.toString()}};
    if (!companionUrl.isEmpty()) message.insert(QStringLiteral("companionUrl"), companionUrl.toString());
    if (!recommendedOptions.isEmpty()) message.insert("recommendedOptions", recommendedOptions);
    if (!companionUrls.isEmpty()) message.insert("companionUrls", companionUrls);
    // Animation models are always diffusion models: the service sets their type.
    if (category != ModelCategory::Animation && (modelType != "checkpoint" || !existingModelId.isEmpty()))
        message.insert("modelType", modelType);
    if (!existingModelId.isEmpty()) message.insert("id", existingModelId);
    if (explicitContent) message.insert("explicitContent", *explicitContent);
    if (displayName) message.insert("displayName", displayName->trimmed());
    if (mmprojModelId) message.insert("mmprojModelId", *mmprojModelId);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    emit installsChanged();
    return installId;
}

bool ServiceClient::configureModelStyles(const QString &id, const QList<PromptStyle> &styles)
{
    if (!m_authenticated || id.isEmpty() || !Protocol::validatePromptStyles(styles)) return false;
    const QJsonObject message{{"type", "models.styles.configure"}, {"id", id}, {"styles", Protocol::promptStylesToJson(styles)}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}

bool ServiceClient::configureModel(const QString &id, const QJsonObject &options, std::optional<bool> explicitContent, std::optional<QString> displayName, std::optional<QString> mmprojModelId)
{
    if (!m_authenticated || id.isEmpty()
        || (displayName && (displayName->trimmed().isEmpty() || displayName->size() > 256))) return false;
    QJsonObject message{{"type", "models.configure"}, {"id", id}, {"recommendedOptions", options}};
    if (displayName) message.insert("displayName", displayName->trimmed());
    if (mmprojModelId) message.insert("mmprojModelId", *mmprojModelId);
    if (explicitContent) message.insert("explicitContent", *explicitContent);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}
bool ServiceClient::cancelInstall(const QString &installId)
{
    if (!m_authenticated || installId.isEmpty()) return false;
    const QJsonObject message{{"type", "models.install.cancel"}, {"installId", installId}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}

bool ServiceClient::deleteModel(const QString &id)
{
    if (!m_authenticated || id.isEmpty()) return false;
    const QJsonObject message{{QStringLiteral("type"), QStringLiteral("models.delete")}, {QStringLiteral("id"), id}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}

QString ServiceClient::submitGeneration(const ImageGenerationRequest &request)
{
    if (!request.presetId.isEmpty() && (!m_capabilities.modelRouting || !request.styleId.isEmpty())) return {};
    if (!request.qualityId.isEmpty()) return {};
    if (!request.referenceImages.isEmpty() && (!request.baseImageData.isEmpty() || request.referenceImages.size() > 8
        || request.referenceImages.contains(QByteArray()))) return {};
    QJsonObject options{{QStringLiteral("prompt"), request.prompt},
                              {QStringLiteral("negativePrompt"), request.negativePrompt},
                              {QStringLiteral("sampler"), request.sampler},
                              {QStringLiteral("scheduler"), request.scheduler},
                              {QStringLiteral("width"), request.width},
                              {QStringLiteral("height"), request.height},
                              {QStringLiteral("steps"), request.steps},
                              {QStringLiteral("cfgScale"), request.cfgScale},
                              {QStringLiteral("clipSkip"), request.clipSkip},
                              {QStringLiteral("seed"), request.seed},
                              {QStringLiteral("hasInput"), !request.baseImageData.isEmpty()},
                              {QStringLiteral("baseImageStrength"), request.baseImageStrength},
                              {QStringLiteral("modelId"), request.modelId}};
    if (!request.styleId.isEmpty()) options.insert("styleId", request.styleId);
    if (!request.presetId.isEmpty()) options.insert("presetId", request.presetId);
    if (!request.referenceImages.isEmpty()) {
        options.insert("inputCount", int(request.referenceImages.size()));
        return submit(ImageOperation::Generate, options, request.referenceImages);
    }
    return submit(ImageOperation::Generate, options, request.baseImageData.isEmpty() ? QList<QByteArray>{} : QList<QByteArray>{request.baseImageData});
}

QString ServiceClient::submitEdit(const ImageGenerationRequest &request)
{
    if (!request.styleId.isEmpty() || !request.presetId.isEmpty()
        || (!request.qualityId.isEmpty() && !m_capabilities.modelRouting)) return {};
    if (request.editImages.isEmpty() || request.editImages.size() > 8 || request.editImages.contains(QByteArray()) || request.prompt.trimmed().isEmpty()) return {};
    QJsonObject options{{"modelId", request.modelId}, {"inputCount", int(request.editImages.size())}, {"prompt", request.prompt},
         {"width", request.width}, {"height", request.height}, {"steps", request.steps},
         {"cfgScale", request.cfgScale}, {"sampler", request.sampler},
         {"scheduler", request.scheduler}, {"clipSkip", request.clipSkip}};
    if (!request.qualityId.isEmpty()) options.insert("qualityId", request.qualityId);
    if (!request.negativePrompt.isEmpty()) options.insert("negativePrompt", request.negativePrompt);
    return submit(ImageOperation::Edit, options, request.editImages);
}

QString ServiceClient::submitUpscale(const ImageUpscaleRequest &request)
{
    if (request.sourceImageData.isEmpty()) return {};
    return submit(ImageOperation::Upscale,
                  {{QStringLiteral("modelId"), request.modelId}, {QStringLiteral("hasInput"), true}},
                  {request.sourceImageData});
}

bool ServiceClient::canEnhancePrompt(const QString& modelId) const
{
    return m_authenticated && m_capabilities.promptEnhancement
        && containsModel(modelId, ModelCategory::LlmChat);
}

QString ServiceClient::enhancePrompt(const PromptEnhancementRequest& request)
{
    if (!canEnhancePrompt(request.modelId)) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.promptEnhancement = true;
    m_pendingJobs.insert(jobId, job);
    QJsonObject options{{"modelId", request.modelId}, {"prompt", request.prompt}};
    if (!request.parameters.isEmpty()) options.insert("parameters", request.parameters);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"jobId", jobId}, {"operation", "prompt.enhance"}, {"options", options}}).toJson(QJsonDocument::Compact)));
    return jobId;
}

bool ServiceClient::canChat(const QString& modelId) const
{
    return m_authenticated && m_capabilities.textChat && containsModel(modelId, ModelCategory::LlmChat);
}

bool ServiceClient::canChatWithImages(const QString& modelId) const
{
    if (!canChat(modelId) || !m_capabilities.textChatImages) return false;
    for (const ModelInfo& model : m_models)
        if (model.id == modelId) return !model.mmprojModelId.isEmpty();
    return false;
}

QString ServiceClient::submitChat(const TextChatRequest& request)
{
    if (!canChat(request.modelId) || request.messages.isEmpty()) return {};
    if (!request.images.isEmpty() && (!canChatWithImages(request.modelId) || request.images.size() > MaximumChatImages))
        return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.textChat = true;
    job.inputImages = request.images;
    m_pendingJobs.insert(jobId, job);
    QJsonArray messages;
    for (const auto& message : request.messages)
        messages.append(QJsonObject{{"role", message.role}, {"content", message.content}});
    QJsonObject options{{"modelId", request.modelId}, {"messages", messages}};
    // The service asks for each image through upload-required, then shows them with the last user message.
    if (!request.images.isEmpty()) options.insert("imageCount", int(request.images.size()));
    if (!request.parameters.isEmpty()) options.insert("parameters", request.parameters);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"jobId", jobId}, {"operation", "text.chat"}, {"options", options}}).toJson(QJsonDocument::Compact)));
    return jobId;
}

QString ServiceClient::loadChatModel(const QString& modelId, const QJsonObject& parameters)
{
    if (!m_capabilities.chatModelControl || !canChat(modelId)) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.chatModelOperation = "text.chat.load";
    m_pendingJobs.insert(jobId, job);
    QJsonObject options{{"modelId", modelId}};
    if (!parameters.isEmpty()) options.insert("parameters", parameters);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"jobId", jobId}, {"operation", "text.chat.load"}, {"options", options}}).toJson(QJsonDocument::Compact)));
    return jobId;
}

QString ServiceClient::unloadChatModel()
{
    if (!m_authenticated || !m_capabilities.chatModelControl) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.chatModelOperation = "text.chat.unload";
    m_pendingJobs.insert(jobId, job);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"jobId", jobId}, {"operation", "text.chat.unload"}}).toJson(QJsonDocument::Compact)));
    return jobId;
}

bool ServiceClient::canAnimate() const
{
    return m_authenticated && m_capabilities.imageAnimate;
}

bool ServiceClient::canPingPong() const
{
    return canAnimate() && m_capabilities.imageAnimatePingPong;
}

bool ServiceClient::canAnimateMp4() const
{
    return canAnimate() && m_capabilities.imageAnimateMp4;
}

QString ServiceClient::submitAnimation(const AnimationRequest &request)
{
    if (!canAnimate() || !containsModel(request.modelId, ModelCategory::Animation)
        || request.prompt.trimmed().isEmpty() || request.startImage.isEmpty()) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.animation = true;
    // The service asks for the start image (index 0), then the end image (index 1).
    job.inputImages = {request.startImage};
    if (!request.endImage.isEmpty()) job.inputImages.append(request.endImage);
    m_pendingJobs.insert(jobId, job);
    QJsonObject options{{"modelId", request.modelId}, {"prompt", request.prompt}, {"seed", request.seed},
                        {"hasEndImage", !request.endImage.isEmpty()}};
    if (!request.negativePrompt.isEmpty()) options.insert("negativePrompt", request.negativePrompt);
    if (!request.parameters.isEmpty()) options.insert("parameters", request.parameters);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"jobId", jobId}, {"operation", "image.animate"}, {"options", options}}).toJson(QJsonDocument::Compact)));
    return jobId;
}

QString ServiceClient::submit(ImageOperation operation, const QJsonObject &options, const QList<QByteArray> &inputImages)
{
    QString modelId = options.value("modelId").toString();
    if (modelId.isEmpty() && m_routing && operation == ImageOperation::Generate && options.contains("presetId"))
        for (const auto &preset : m_routing->presets)
            if (preset.id == options.value("presetId").toString()) { modelId = preset.model.modelId; break; }
    if (modelId.isEmpty() && m_routing && operation == ImageOperation::Edit && options.contains("qualityId"))
        if (const auto *quality = m_routing->editingQuality(options.value("qualityId").toString()))
            modelId = quality->model.modelId;
    if (!canSubmit(operation, modelId)) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.operation = operation;
    job.inputImages = inputImages;
    m_pendingJobs.insert(jobId, job);
    const QJsonObject message{{QStringLiteral("type"), QStringLiteral("submit")},
                              {QStringLiteral("jobId"), jobId},
                              {QStringLiteral("operation"), operationName(operation)},
                              {QStringLiteral("options"), options}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return jobId;
}

void ServiceClient::authenticate()
{
    const QJsonObject message{{QStringLiteral("type"), QStringLiteral("authenticate")},
                              {QStringLiteral("version"), Protocol::Version},
                              {QStringLiteral("token"), m_token}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void ServiceClient::handleTextMessage(const QString &message)
{
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) return;
    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("authenticated")) {
        m_authenticated = true;
        requestModels();
        emit serviceStateChanged(ServiceConnectionState::Authenticated, QStringLiteral("authenticated"), {});
        return;
    }
    if (type == QStringLiteral("capabilities")) {
        const QJsonArray operations = object.value(QStringLiteral("operations")).toArray();
        QStringList chatEngines;
        for (const auto& engine : object.value("textChatEngines").toArray())
            if (engine.isString()) chatEngines.append(engine.toString());
        updateCapabilities({operations.contains("image.generate"), operations.contains("image.upscale"),
            operations.contains("image.edit"), operations.contains("prompt.enhance"), operations.contains("model-routing"),
            operations.contains("text.chat"), chatEngines,
            operations.contains("text.chat.load") && operations.contains("text.chat.unload"),
            operations.contains("text.chat.images"), operations.contains("image.animate"),
            operations.contains("image.animate.pingPong"), operations.contains("image.animate.mp4")});
        if (m_capabilities.modelRouting) requestModelRouting();
        const QString state = object.value(QStringLiteral("state")).toString();
        emit serviceStateChanged(state == QStringLiteral("ready") ? ServiceConnectionState::Ready : ServiceConnectionState::Authenticated, state, {});
        return;
    }
    if (type == QStringLiteral("models.list.result")) {
        m_models.clear();
        for (const QJsonValue &categoryValue : object.value(QStringLiteral("categories")).toArray()) {
            const QJsonObject categoryObject = categoryValue.toObject();
            const ModelCategory category = categoryFromName(categoryObject.value(QStringLiteral("category")).toString());
            for (const QJsonValue &modelValue : categoryObject.value(QStringLiteral("models")).toArray()) {
                const QJsonObject model = modelValue.toObject();
                m_models.append({model.value(QStringLiteral("id")).toString(),
                                 model.value(QStringLiteral("name")).toString(),
                                 model.value(QStringLiteral("fileName")).toString(),
                                 model.value(QStringLiteral("bytes")).toVariant().toLongLong(),
                                 category,
                                 model.value(QStringLiteral("companionFileName")).toString(),
                                 model.value(QStringLiteral("companionRole")).toString(), model.value("recommendedOptions").toObject(),
                    model.value("companionFiles").toObject(), model.value("modelType").toString("checkpoint"), model.value("explicitContent").toBool(false),
                    Protocol::promptStylesFromJson(model.value("styles")).value_or(QList<PromptStyle>{}), model.value("displayName").toString().trimmed(), model.value("mmprojModelId").toString(),
                    model.value("missingFiles").toVariant().toStringList()});
            }
        }
        m_modelListAvailable = true;
        emit modelsChanged();
        return;
    }
    if (type == QStringLiteral("models.install.accepted")) {
        if (ModelInstall *install = findInstall(object.value(QStringLiteral("installId")).toString())) {
            install->state = ModelInstallState::Accepted;
            emit installsChanged();
        }
        return;
    }
    if (type == QStringLiteral("models.install.progress")) {
        if (ModelInstall *install = findInstall(object.value(QStringLiteral("installId")).toString())) {
            const QString state = object.value(QStringLiteral("state")).toString();
            install->state = state == QStringLiteral("queued") ? ModelInstallState::Queued
                : state == QStringLiteral("verifying") ? ModelInstallState::Verifying : ModelInstallState::Downloading;
            install->bytesDownloaded = object.value(QStringLiteral("bytesDownloaded")).toVariant().toLongLong();
            install->totalBytes = object.contains(QStringLiteral("totalBytes"))
                ? std::optional<qint64>(object.value(QStringLiteral("totalBytes")).toVariant().toLongLong()) : std::nullopt;
            emit installsChanged();
        }
        return;
    }
    if (type == QStringLiteral("models.install.completed")) {
        if (ModelInstall *install = findInstall(object.value(QStringLiteral("installId")).toString())) {
            install->state = ModelInstallState::Completed;
            install->message = tr("Installed");
            emit installsChanged();
        }
        requestModels();
        return;
    }
    if (type == "models.install.canceled") {
        if (auto *install = findInstall(object.value("installId").toString())) { install->state = ModelInstallState::Canceled; emit installsChanged(); }
        return;
    }
    if (type == "models.configure.completed") {
        emit modelConfigured(object.value("id").toString(), object.value("model").toObject().value("recommendedOptions").toObject());
        requestModels();
        return;
    }
    if (type == "models.styles.configure.completed") {
        const auto styles = Protocol::promptStylesFromJson(object.value("styles"));
        if (styles) emit modelStylesConfigured(object.value("id").toString(), *styles);
        return;
    }
    if (type == "model-routing.changed") { requestModelRouting(); return; }
    if (type == "model-routing.get.completed" || type == "model-routing.configure.completed") {
        QString error;
        m_routing = Protocol::modelRoutingFromJson(object.value("routing").toObject(), &error);
        emit modelRoutingChanged();
        if (!m_routing) emit modelRoutingFailed(error);
        else if (type == "model-routing.configure.completed") emit modelRoutingConfigured();
        return;
    }
    if (type == "models.styles.changed" || type == "models.changed") { requestModels(); return; }
    if (type == "models.companion.delete.completed") {
        emit companionDeleted(object.value("id").toString(), object.value("role").toString());
        requestModels(); return;
    }
    if (type == QStringLiteral("models.delete.completed")) {
        emit modelDeleted(object.value(QStringLiteral("id")).toString());
        requestModels();
        return;
    }
    const QString jobId = object.value(QStringLiteral("jobId")).toString();
    if (type == QStringLiteral("error")) {
        const QString code = object.value(QStringLiteral("code")).toString();
        const QString detail = object.value(QStringLiteral("message")).toString();
        if (object.value("operation").toString().startsWith("model-routing.")) {
            if (object.value("operation") == "model-routing.get") { m_routing.reset(); emit modelRoutingChanged(); }
            emit modelRoutingFailed(detail);
            return;
        }
        if (ModelInstall *install = findInstall(object.value(QStringLiteral("installId")).toString())) {
            if (install->state == ModelInstallState::Completed || install->state == ModelInstallState::Canceled) return;
            install->state = ModelInstallState::Failed;
            install->message = detail;
            emit installsChanged();
        } else if (!object.value(QStringLiteral("id")).toString().isEmpty()) {
            emit modelOperationFailed(object.value(QStringLiteral("id")).toString(), detail);
        } else if (!m_authenticated && code == QStringLiteral("authentication-failed")) {
            m_connectionErrorCode = code;
            m_connectionErrorDetail = detail;
            m_socket->close();
        } else if (!jobId.isEmpty()) {
            failJob(jobId, code, detail);
        } else {
            emit serviceStateChanged(ServiceConnectionState::Error, code, detail);
        }
        return;
    }
    if (!m_pendingJobs.contains(jobId)) return;
    if (type == QStringLiteral("upload-required")) {
        emit jobStateChanged(jobId, JobState::Uploading, -1);
        const auto &job = m_pendingJobs.value(jobId);
        const int index = object.value("inputIndex").toInt(0);
        if (index < 0 || index >= job.inputImages.size()) return;
        m_socket->sendBinaryMessage(job.inputImages.at(index));
    } else if (type == QStringLiteral("progress")) {
        emit generationProgressChanged(jobId, Protocol::progressFromJson(object.value(QStringLiteral("progress")).toObject()));
    } else if (type == QStringLiteral("output")) {
        emit jobOutput(jobId, object.value(QStringLiteral("stream")).toString(), object.value(QStringLiteral("text")).toString());
    } else if (type == QStringLiteral("queued")) {
        emit jobStateChanged(jobId, JobState::Queued, -1);
    } else if (type == QStringLiteral("queue-position")) {
        emit jobStateChanged(jobId, JobState::Queued, object.value(QStringLiteral("position")).toInt());
    } else if (type == QStringLiteral("running")) {
        emit jobStateChanged(jobId, JobState::Running, -1);
    } else if (type == QStringLiteral("result")) {
        if (m_pendingJobs.value(jobId).promptEnhancement) {
            if (object.value("operation") != "prompt.enhance" || !object.value("prompt").isString()
                || object.value("prompt").toString().trimmed().isEmpty()) {
                failJob(jobId, "invalid-result", tr("The service returned an invalid enhanced prompt"));
                return;
            }
            m_pendingJobs.remove(jobId);
            emit jobStateChanged(jobId, JobState::Completed, -1);
            emit promptEnhancementFinished({jobId, true, object.value("prompt").toString(), {}, {}});
            return;
        }
        if (const QString operation = m_pendingJobs.value(jobId).chatModelOperation; !operation.isEmpty()) {
            if (object.value("operation") != operation) {
                failJob(jobId, "invalid-result", tr("The service returned an invalid chat model result"));
                return;
            }
            m_pendingJobs.remove(jobId);
            emit jobStateChanged(jobId, JobState::Completed, -1);
            ChatModelResult result;
            result.jobId = jobId;
            result.load = operation == "text.chat.load";
            result.succeeded = true;
            result.coldStart = object.value("coldStart").toBool();
            result.unloaded = object.value("unloaded").toBool();
            result.durationMs = object.value("durationMs").toInteger(-1);
            emit chatModelFinished(result);
            return;
        }
        if (m_pendingJobs.value(jobId).textChat) {
            if (object.value("operation") != "text.chat" || !object.value("text").isString()
                || object.value("text").toString().trimmed().isEmpty()) {
                failJob(jobId, "invalid-result", tr("The service returned an invalid chat reply"));
                return;
            }
            const auto usage = object.value("usage").toObject();
            m_pendingJobs.remove(jobId);
            emit jobStateChanged(jobId, JobState::Completed, -1);
            emit chatFinished({jobId, true, object.value("text").toString(), object.value("engine").toString(),
                object.value("finishReason").toString(), usage.value("promptTokens").toInt(-1),
                usage.value("completionTokens").toInt(-1), object.value("durationMs").toInteger(-1),
                object.value("coldStart").toBool(), {}, {}, object.value("json")});
            return;
        }
        auto &job = m_pendingJobs[jobId];
        if (job.animation && object.value("operation") != "image.animate") {
            failJob(jobId, "invalid-result", tr("The service returned an invalid animation result"));
            return;
        }
        job.frames = object.value("frames").toInt();
        job.fps = object.value("fps").toInt();
        job.mimeType = object.value(QStringLiteral("mimeType")).toString();
        job.prompt = object.value(QStringLiteral("prompt")).toString();
        job.negativePrompt = object.value(QStringLiteral("negativePrompt")).toString();
        m_resultJobs.enqueue(jobId);
    }
}

void ServiceClient::handleBinaryMessage(const QByteArray &data)
{
    if (m_resultJobs.isEmpty()) return;
    const QString jobId = m_resultJobs.dequeue();
    const PendingJob job = m_pendingJobs.take(jobId);
    emit jobStateChanged(jobId, JobState::Completed, -1);
    if (job.animation) emit animationFinished({jobId, true, data, job.mimeType, job.frames, job.fps, {}, {}});
    else emit imageJobFinished({jobId, job.operation, true, data, job.mimeType, {}, {}, job.prompt, job.negativePrompt});
}

void ServiceClient::updateCapabilities(const ServiceCapabilities &capabilities)
{
    if (m_capabilities.imageGeneration == capabilities.imageGeneration && m_capabilities.imageUpscale == capabilities.imageUpscale && m_capabilities.imageEdit == capabilities.imageEdit
        && m_capabilities.promptEnhancement == capabilities.promptEnhancement
        && m_capabilities.modelRouting == capabilities.modelRouting
        && m_capabilities.textChat == capabilities.textChat
        && m_capabilities.textChatEngines == capabilities.textChatEngines
        && m_capabilities.chatModelControl == capabilities.chatModelControl
        && m_capabilities.textChatImages == capabilities.textChatImages
        && m_capabilities.imageAnimate == capabilities.imageAnimate
        && m_capabilities.imageAnimatePingPong == capabilities.imageAnimatePingPong
        && m_capabilities.imageAnimateMp4 == capabilities.imageAnimateMp4) return;
    m_capabilities = capabilities;
    emit capabilitiesChanged(m_capabilities);
}

void ServiceClient::clearCatalog()
{
    const bool changed = m_modelListAvailable || !m_models.isEmpty();
    m_modelListAvailable = false;
    m_models.clear();
    m_routing.reset();
    emit modelRoutingChanged();
    if (changed) emit modelsChanged();
}

std::optional<ModelRouting> ServiceClient::modelRouting() const { return m_routing; }

void ServiceClient::requestModelRouting()
{
    if (!m_authenticated || !m_capabilities.modelRouting) return;
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "model-routing.get"}}).toJson(QJsonDocument::Compact)));
}

bool ServiceClient::configureModelRouting(const ModelRouting &routing)
{
    if (!m_authenticated || !m_capabilities.modelRouting || !Protocol::validateModelRouting(routing)) return false;
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "model-routing.configure"},
        {"routing", Protocol::modelRoutingToJson(routing)}}).toJson(QJsonDocument::Compact)));
    return true;
}

ModelInstall *ServiceClient::findInstall(const QString &installId)
{
    for (ModelInstall &install : m_installs) if (install.installId == installId) return &install;
    return nullptr;
}

void ServiceClient::failActiveInstalls(const QString &message)
{
    bool changed = false;
    for (ModelInstall &install : m_installs) {
        if (install.state == ModelInstallState::Completed || install.state == ModelInstallState::Failed || install.state == ModelInstallState::Canceled) continue;
        install.state = ModelInstallState::Failed;
        install.message = message;
        changed = true;
    }
    if (changed) emit installsChanged();
}

void ServiceClient::failJob(const QString &jobId, const QString &errorCode, const QString &message)
{
    if (!m_pendingJobs.contains(jobId)) return;
    const PendingJob job = m_pendingJobs.take(jobId);
    m_resultJobs.removeAll(jobId);
    emit jobStateChanged(jobId, JobState::Failed, -1);
    if (job.promptEnhancement) emit promptEnhancementFinished({jobId, false, {}, errorCode, message});
    else if (job.textChat) emit chatFinished({jobId, false, {}, {}, {}, -1, -1, -1, false, errorCode, message, {}});
    else if (!job.chatModelOperation.isEmpty()) {
        ChatModelResult result;
        result.jobId = jobId;
        result.load = job.chatModelOperation == "text.chat.load";
        result.errorCode = errorCode;
        result.message = message;
        emit chatModelFinished(result);
    }
    else if (job.animation) emit animationFinished({jobId, false, {}, {}, 0, 0, errorCode, message});
    else emit imageJobFinished({jobId, job.operation, false, {}, job.mimeType, errorCode, message});
}

void ServiceClient::failAllJobs(const QString &errorCode, const QString &message)
{
    const QStringList jobIds = m_pendingJobs.keys();
    for (const QString &jobId : jobIds) failJob(jobId, errorCode, message);
    m_resultJobs.clear();
}

bool ServiceClient::containsModel(const QString &modelId, ModelCategory category) const
{
    for (const ModelInfo &model : m_models) {
        if (model.id == modelId && model.category == category) return true;
    }
    return false;
}
}

bool Nukleus::ServiceClient::deleteCompanion(const QString& id, const QString& role)
{
    if (!m_authenticated || id.isEmpty() || role.isEmpty()) return false;
    const QJsonObject message{{"type", "models.companion.delete"}, {"id", id}, {"role", role}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}
