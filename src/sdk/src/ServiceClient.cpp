#include "kira/service/ServiceClient.h"
#include "kira/service/ServiceProtocol.h"
#include "kira/service/PhotoMakerIdentity.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QWebSocket>
#include <algorithm>

namespace
{
KiraService::ModelCategory categoryFromName(const QString &name)
{
    if (name == QStringLiteral("face-parsing")) return KiraService::ModelCategory::FaceParsing;
    if (name == QStringLiteral("inpaint")) return KiraService::ModelCategory::Inpaint;
    if (name == QStringLiteral("image-generation")) return KiraService::ModelCategory::ImageGeneration;
    if (name == QStringLiteral("upscaler")) return KiraService::ModelCategory::Upscaler;
    if (name == QStringLiteral("llm-chat")) return KiraService::ModelCategory::LlmChat;
    if (name == QStringLiteral("ip-adapter") || name == QStringLiteral("ip_adapter")) return KiraService::ModelCategory::IpAdapter;
    if (name == QStringLiteral("edition")) return KiraService::ModelCategory::Edition;
    if (name == "clip_vision") return KiraService::ModelCategory::ClipVision;
    if (name == "photomaker") return KiraService::ModelCategory::PhotoMaker;
    if (name == "prompt-enhancer") return KiraService::ModelCategory::PromptEnhancer;
    return KiraService::ModelCategory::Unknown;
}

QString categoryName(KiraService::ModelCategory category)
{
    if (category == KiraService::ModelCategory::FaceParsing) return QStringLiteral("face-parsing");
    if (category == KiraService::ModelCategory::Inpaint) return QStringLiteral("inpaint");
    if (category == KiraService::ModelCategory::ImageGeneration) return QStringLiteral("image-generation");
    if (category == KiraService::ModelCategory::Upscaler) return QStringLiteral("upscaler");
    if (category == KiraService::ModelCategory::LlmChat) return QStringLiteral("llm-chat");
    if (category == KiraService::ModelCategory::Edition) return QStringLiteral("edition");
    // Retain the accepted alias so existing SDK installation calls also work with older servers.
    if (category == KiraService::ModelCategory::IpAdapter) return QStringLiteral("ip-adapter");
    if (category == KiraService::ModelCategory::ClipVision) return QStringLiteral("clip_vision");
    if (category == KiraService::ModelCategory::PhotoMaker) return QStringLiteral("photomaker");
    if (category == KiraService::ModelCategory::PromptEnhancer) return QStringLiteral("prompt-enhancer");
    return {};
}

void addIpAdapterOptions(QJsonObject &options, const KiraService::ImageGenerationRequest &request)
{
    if (!request.clipVisionModelId.isEmpty()) options.insert("clipVisionModelId", request.clipVisionModelId);
    if (!request.ipAdapterModelId.isEmpty()) options.insert("ipAdapterModelId", request.ipAdapterModelId);
    if (!request.ipAdapterImageData.isEmpty()) options.insert("hasIpAdapterImage", true);
    if (request.ipAdapterStrength) options.insert("ip_adapter_strength", *request.ipAdapterStrength);
}

QString operationName(KiraService::ImageOperation operation)
{
    if (operation == KiraService::ImageOperation::FaceParsing) return QStringLiteral("image.face-parsing");
    if (operation == KiraService::ImageOperation::Inpaint) return QStringLiteral("image.inpaint");
    if (operation == KiraService::ImageOperation::Edit) return QStringLiteral("image.edit");
    return operation == KiraService::ImageOperation::Generate ? QStringLiteral("image.generate") : QStringLiteral("image.upscale");
}
}

namespace KiraService
{
ServiceClient::ServiceClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QStringLiteral("Kira Service SDK"), QWebSocketProtocol::VersionLatest, this))
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
    if (operation == ImageOperation::FaceParsing) return m_capabilities.faceParsing;
    if (operation == ImageOperation::Inpaint) return m_capabilities.imageInpaint;
    if (operation == ImageOperation::Edit) return m_capabilities.imageEdit;
    return operation == ImageOperation::Generate ? m_capabilities.imageGeneration : m_capabilities.imageUpscale;
}

bool ServiceClient::canSubmit(ImageOperation operation, const QString &modelId) const
{
    const ModelCategory category = operation == ImageOperation::Inpaint ? ModelCategory::Inpaint
        : operation == ImageOperation::FaceParsing ? ModelCategory::FaceParsing
        : operation == ImageOperation::Edit ? ModelCategory::Edition
        : operation == ImageOperation::Generate ? ModelCategory::ImageGeneration : ModelCategory::Upscaler;
    return m_authenticated && supports(operation) && containsModel(modelId, category);
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

QList<ModelInstall> ServiceClient::installs() const { return m_installs; }

void ServiceClient::requestModels()
{
    if (m_authenticated) m_socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list\"}"));
}

QString ServiceClient::installModel(ModelCategory category, const QUrl &url, const QUrl &companionUrl, const QJsonObject &recommendedOptions, const QJsonObject &companionUrls,
    const QString &modelType, const QString &existingModelId, std::optional<bool> explicitContent, std::optional<QString> displayName)
{
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
    if (modelType != "checkpoint" || !existingModelId.isEmpty()) message.insert("modelType", modelType);
    if (!existingModelId.isEmpty()) message.insert("id", existingModelId);
    if (explicitContent) message.insert("explicitContent", *explicitContent);
    if (displayName) message.insert("displayName", displayName->trimmed());
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

bool ServiceClient::configureModel(const QString &id, const QJsonObject &options, std::optional<bool> explicitContent, std::optional<QString> displayName)
{
    if (!m_authenticated || id.isEmpty()) return false;
    QJsonObject message{{"type", "models.configure"}, {"id", id}, {"recommendedOptions", options}};
    if (displayName) message.insert("displayName", displayName->trimmed());
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
    if (!request.photoMakerIdentityPackageData.isEmpty()) {
        if (!request.photoMakerIdentityData.isEmpty() || !request.photoMakerReferenceImageData.isEmpty()
            || !request.photoMakerReferenceImages.isEmpty()) return {};
        auto unpacked = request;
        PhotoMakerIdentityPackage identity;
        if (!unpackPhotoMakerIdentity(request.photoMakerIdentityPackageData, identity)) return {};
        unpacked.photoMakerReferenceImages = identity.references;
        unpacked.photoMakerIdentityData = identity.embeddings;
        unpacked.photoMakerIdentityPackageData.clear();
        return submitGeneration(unpacked);
    }
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
    addIpAdapterOptions(options, request);
    if (!request.styleId.isEmpty()) options.insert("styleId", request.styleId);
    const bool photoMaker = !request.photoMakerModelId.isEmpty() || !request.photoMakerIdentityData.isEmpty()
        || !request.photoMakerReferenceImageData.isEmpty() || !request.photoMakerReferenceImages.isEmpty() || request.photoMakerStyleStrength.has_value();
    const auto references = request.photoMakerReferenceImages.isEmpty()
        ? QList<QByteArray>{request.photoMakerReferenceImageData} : request.photoMakerReferenceImages;
    if (photoMaker) {
        if (!m_capabilities.photoMaker || !request.baseImageData.isEmpty()
            || !request.ipAdapterModelId.isEmpty() || !request.clipVisionModelId.isEmpty()
            || !request.ipAdapterImageData.isEmpty() || request.ipAdapterStrength.has_value()
            || request.photoMakerIdentityData.isEmpty() || references.isEmpty() || references.size() > 4
            || std::any_of(references.cbegin(), references.cend(), [](const QByteArray& image) { return image.isEmpty(); })
            || (!request.photoMakerReferenceImages.isEmpty() && !request.photoMakerReferenceImageData.isEmpty())
            || !containsModel(request.photoMakerModelId, ModelCategory::PhotoMaker)
            || (request.photoMakerStyleStrength && (*request.photoMakerStyleStrength < 0 || *request.photoMakerStyleStrength > 100))) return {};
        options.insert("photoMakerModelId", request.photoMakerModelId);
        options.insert("hasPhotoMakerIdentity", true);
        if (references.size() > 1) options.insert("referenceCount", references.size());
        if (request.photoMakerStyleStrength) options.insert("photoMakerStyleStrength", *request.photoMakerStyleStrength);
    }
    return submit(ImageOperation::Generate, options,
        photoMaker ? references.first() : request.baseImageData,
        request.ipAdapterImageData, request.photoMakerIdentityData, photoMaker ? references : QList<QByteArray>{});
}

QString ServiceClient::submitEdit(const ImageGenerationRequest &request)
{
    if (!request.styleId.isEmpty()) return {};
    if (!request.photoMakerIdentityPackageData.isEmpty()) return {};
    if (!request.photoMakerModelId.isEmpty() || !request.photoMakerIdentityData.isEmpty()
        || !request.photoMakerReferenceImageData.isEmpty() || !request.photoMakerReferenceImages.isEmpty()
        || request.photoMakerStyleStrength.has_value()) return {};
    if (request.baseImageData.isEmpty() || request.prompt.trimmed().isEmpty()) return {};
    QJsonObject options{{"modelId", request.modelId}, {"hasInput", true}, {"prompt", request.prompt},
         {"width", request.width}, {"height", request.height}, {"steps", request.steps},
         {"cfgScale", request.cfgScale}, {"sampler", request.sampler},
         {"scheduler", request.scheduler}, {"clipSkip", request.clipSkip}};
    addIpAdapterOptions(options, request);
    return submit(ImageOperation::Edit, options, request.baseImageData, request.ipAdapterImageData);
}

QString ServiceClient::submitUpscale(const ImageUpscaleRequest &request)
{
    if (request.sourceImageData.isEmpty()) return {};
    return submit(ImageOperation::Upscale,
                  {{QStringLiteral("modelId"), request.modelId}, {QStringLiteral("hasInput"), true}},
                  request.sourceImageData);
}

QString ServiceClient::submitFaceParsing(const ImageUpscaleRequest& request)
{
    if (request.sourceImageData.isEmpty()) return {};
    return submit(ImageOperation::FaceParsing, {{"modelId", request.modelId}}, request.sourceImageData);
}

QString ServiceClient::submitInpaint(const ImageGenerationRequest& request, const QByteArray& maskImageData)
{
    if (request.baseImageData.isEmpty() || maskImageData.isEmpty() || request.prompt.trimmed().isEmpty()
        || !request.styleId.isEmpty() || !request.photoMakerModelId.isEmpty()
        || !request.photoMakerIdentityPackageData.isEmpty() || !request.photoMakerIdentityData.isEmpty()
        || !request.photoMakerReferenceImageData.isEmpty() || !request.photoMakerReferenceImages.isEmpty()
        || request.photoMakerStyleStrength) return {};
    QJsonObject options{{"modelId", request.modelId}, {"hasInput", true}, {"hasMask", true},
        {"prompt", request.prompt}, {"negativePrompt", request.negativePrompt},
        {"width", request.width}, {"height", request.height}, {"steps", request.steps},
        {"cfgScale", request.cfgScale}, {"seed", request.seed}, {"clipSkip", request.clipSkip},
        {"sampler", request.sampler}, {"scheduler", request.scheduler},
        {"baseImageStrength", request.baseImageStrength}};
    addIpAdapterOptions(options, request);
    return submit(ImageOperation::Inpaint, options, request.baseImageData, request.ipAdapterImageData, {}, {}, maskImageData);
}

bool ServiceClient::canEnhancePrompt(const QString& modelId) const
{
    return m_authenticated && m_capabilities.promptEnhancement
        && containsModel(modelId, ModelCategory::PromptEnhancer);
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

QString ServiceClient::exportPhotoMakerIdentity(const QByteArray &sourceImageData)
{
    return exportPhotoMakerIdentityImages(QList<QByteArray>{sourceImageData});
}

QString ServiceClient::exportPhotoMakerIdentityImages(const QList<QByteArray>& sourceImages)
{
    if (!m_authenticated || !m_capabilities.photoMakerIdentityExport || sourceImages.isEmpty() || sourceImages.size() > 4
        || std::any_of(sourceImages.cbegin(), sourceImages.cend(), [](const QByteArray& image) { return image.isEmpty(); })) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.inputData = sourceImages.first();
    job.referenceImages = sourceImages;
    job.identityExport = true;
    m_pendingJobs.insert(jobId, job);
    const QJsonObject message{{"type", "submit"}, {"jobId", jobId}, {"operation", "photomaker.export"},
        {"options", sourceImages.size() == 1 ? QJsonObject{} : QJsonObject{{"referenceCount", sourceImages.size()}}}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return jobId;
}

QString ServiceClient::submit(ImageOperation operation, const QJsonObject &options, const QByteArray &inputData, const QByteArray &ipAdapterImageData, const QByteArray &photoMakerIdentityData, const QList<QByteArray>& references, const QByteArray& maskImageData)
{
    if (!canSubmit(operation, options.value(QStringLiteral("modelId")).toString())) return {};
    const bool adapterRequested = options.contains("clipVisionModelId") || options.contains("ipAdapterModelId")
        || options.contains("hasIpAdapterImage") || options.contains("ip_adapter_strength");
    if (adapterRequested && (!m_capabilities.ipAdapter || ipAdapterImageData.isEmpty()
        || !containsModel(options.value("clipVisionModelId").toString(), ModelCategory::ClipVision)
        || !containsModel(options.value("ipAdapterModelId").toString(), ModelCategory::IpAdapter))) return {};
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingJob job;
    job.operation = operation;
    job.inputData = inputData;
    job.ipAdapterImageData = ipAdapterImageData;
    job.maskImageData = maskImageData;
    job.photoMakerIdentityData = photoMakerIdentityData;
    job.referenceImages = references;
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
        updateCapabilities({operations.contains(QStringLiteral("image.generate")), operations.contains(QStringLiteral("image.upscale")), operations.contains(QStringLiteral("image.edit")), object.value("ipAdapter").toBool(false),
            operations.contains("photomaker.export"), object.value("photoMaker").toBool(false), operations.contains("prompt.enhance"), operations.contains("image.face-parsing"), operations.contains("image.inpaint")});
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
                    Protocol::promptStylesFromJson(model.value("styles")).value_or(QList<PromptStyle>{}), model.value("displayName").toString().trimmed()});
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
    if (type == "models.styles.changed") { requestModels(); return; }
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
        const auto role = object.value("inputRole").toString();
        if (object.contains("referenceIndex")) {
            const int index = object.value("referenceIndex").toInt(-1);
            if (index < 0 || index >= job.referenceImages.size()) {
                failJob(jobId, "invalid-reference-index", "The service requested an invalid reference image.");
                return;
            }
            m_socket->sendBinaryMessage(job.referenceImages[index]);
            return;
        }
        m_socket->sendBinaryMessage(role == "photomaker-identity" ? job.photoMakerIdentityData
            : role == "ip-adapter" ? job.ipAdapterImageData : role == "mask" ? job.maskImageData : job.inputData);
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
        m_pendingJobs[jobId].mimeType = object.value(QStringLiteral("mimeType")).toString();
        m_resultJobs.enqueue(jobId);
    }
}

void ServiceClient::handleBinaryMessage(const QByteArray &data)
{
    if (m_resultJobs.isEmpty()) return;
    const QString jobId = m_resultJobs.dequeue();
    const PendingJob job = m_pendingJobs.take(jobId);
    emit jobStateChanged(jobId, JobState::Completed, -1);
    if (job.identityExport) emit photoMakerIdentityExportFinished({jobId, true, data, {}, {}});
    else emit imageJobFinished({jobId, job.operation, true, data, job.mimeType, {}, {}});
}

void ServiceClient::updateCapabilities(const ServiceCapabilities &capabilities)
{
    if (m_capabilities.imageGeneration == capabilities.imageGeneration && m_capabilities.imageUpscale == capabilities.imageUpscale && m_capabilities.imageEdit == capabilities.imageEdit
        && m_capabilities.ipAdapter == capabilities.ipAdapter
        && m_capabilities.photoMaker == capabilities.photoMaker
        && m_capabilities.photoMakerIdentityExport == capabilities.photoMakerIdentityExport
        && m_capabilities.promptEnhancement == capabilities.promptEnhancement
        && m_capabilities.faceParsing == capabilities.faceParsing
        && m_capabilities.imageInpaint == capabilities.imageInpaint) return;
    m_capabilities = capabilities;
    emit capabilitiesChanged(m_capabilities);
}

void ServiceClient::clearCatalog()
{
    const bool changed = m_modelListAvailable || !m_models.isEmpty();
    m_modelListAvailable = false;
    m_models.clear();
    if (changed) emit modelsChanged();
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
    else if (job.identityExport) emit photoMakerIdentityExportFinished({jobId, false, {}, errorCode, message});
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

bool KiraService::ServiceClient::deleteCompanion(const QString& id, const QString& role)
{
    if (!m_authenticated || id.isEmpty() || role.isEmpty()) return false;
    const QJsonObject message{{"type", "models.companion.delete"}, {"id", id}, {"role", role}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}
