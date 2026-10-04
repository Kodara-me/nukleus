#include "ModelCatalog.h"

#include "ServicePaths.h"
#include <kira/service/ModelOptions.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace
{
constexpr int MaximumRedirects = 8;

bool isSafeFileName(const QString &fileName)
{
    return !fileName.isEmpty() && fileName != QStringLiteral(".") && fileName != QStringLiteral("..")
           && QFileInfo(fileName).fileName() == fileName && !fileName.contains(QLatin1Char('\0'));
}

QStringList extensionsFor(ModelCategory category)
{
    switch (category) {
    case ModelCategory::ImageGeneration:
    case ModelCategory::Edition:
        return {QStringLiteral("safetensors"), QStringLiteral("ckpt"), QStringLiteral("gguf")};
    case ModelCategory::IpAdapter:
        return {QStringLiteral("bin"), QStringLiteral("safetensors"), QStringLiteral("ckpt"), QStringLiteral("gguf")};
    case ModelCategory::Upscaler:
    case ModelCategory::LlmChat:
        return {QStringLiteral("gguf")};
    }
    return {};
}

bool isHuggingFaceHost(const QString &host)
{
    return host.compare(QStringLiteral("huggingface.co"), Qt::CaseInsensitive) == 0
           || host.compare(QStringLiteral("www.huggingface.co"), Qt::CaseInsensitive) == 0;
}

QUrl normalizedModelUrl(const QUrl &url)
{
    if (!isHuggingFaceHost(url.host())) return url;

    const QStringList path = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (path.size() >= 5 && path.at(2) == QStringLiteral("blob")) {
        QUrl normalized = url;
        QStringList normalizedPath = path;
        normalizedPath[2] = QStringLiteral("resolve");
        normalized.setPath(QLatin1Char('/') + normalizedPath.join(QLatin1Char('/')));
        return normalized;
    }
    return url;
}
}

struct ModelCatalog::InstallJob
{
    ModelInstallRequest request;
    QString fileName;
    QString finalPath;
    struct File {
        QString role;
        QUrl url;
        QString relativeName;
        QString finalPath;
        QString partPath;
        bool published = false;
    };
    QList<File> files;
    qsizetype fileIndex = 0;
    QJsonObject companionFiles;
    QString legacyCompanion;
    std::unique_ptr<QFile> partFile;
    QPointer<QNetworkReply> reply;
    qint64 bytesDownloaded = 0;
    std::optional<qint64> totalBytes;
    int redirectCount = 0;
};

ModelCatalog::ModelCatalog(QObject *parent)
    : ModelCatalog({}, false, parent)
{
}

ModelCatalog::ModelCatalog(QString rootPathOverride, bool allowInsecureTestUrls, QObject *parent)
    : QObject(parent)
    , m_allowInsecureTestUrls(allowInsecureTestUrls)
    , m_networkManager(new QNetworkAccessManager(this))
{
    qRegisterMetaType<ModelInstallProgress>();
    if (!rootPathOverride.isEmpty()) m_rootPathOverride = QDir::cleanPath(rootPathOverride);
}

ModelCatalog::~ModelCatalog() = default;

QList<ModelCategory> ModelCatalog::categories()
{
    return {ModelCategory::ImageGeneration, ModelCategory::Edition, ModelCategory::Upscaler, ModelCategory::LlmChat, ModelCategory::IpAdapter};
}

QString ModelCatalog::categoryName(ModelCategory category)
{
    switch (category) {
    case ModelCategory::ImageGeneration: return QStringLiteral("image-generation");
    case ModelCategory::Edition: return QStringLiteral("edition");
    case ModelCategory::Upscaler: return QStringLiteral("upscaler");
    case ModelCategory::LlmChat: return QStringLiteral("llm-chat");
    case ModelCategory::IpAdapter: return QStringLiteral("ip-adapter");
    }
    return {};
}

QString ModelCatalog::companionRole(ModelCategory category)
{
    switch (category) {
    case ModelCategory::IpAdapter: return QStringLiteral("lora");
    case ModelCategory::LlmChat: return QStringLiteral("mmproj");
    case ModelCategory::ImageGeneration:
    case ModelCategory::Edition: return QStringLiteral("vae");
    case ModelCategory::Upscaler: return {};
    }
    return {};
}

std::optional<ModelCategory> ModelCatalog::categoryFromName(const QString &name)
{
    for (const ModelCategory category : categories()) {
        if (name == categoryName(category)) return category;
    }
    return std::nullopt;
}

QList<ModelInfo> ModelCatalog::models(ModelCategory category) const
{
    QList<ModelInfo> models;
    QDir directory(directoryPath(category));
    const QJsonObject metadata = readMetadata(category).value_or(QJsonObject{});
    for (const QFileInfo &fileInfo : directory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name)) {
        if (!acceptsFileName(category, fileInfo.fileName())) continue;
        const QString fileName = fileInfo.fileName();
        const auto entry = metadata.value(fileName);
        const QString companionFileName = entry.isString() ? entry.toString() : entry.toObject().value("companionFileName").toString();
        if (!companionFileName.isEmpty() && companionPath(category, fileName, companionFileName).isEmpty()) continue;
        if (!companionFileName.isEmpty() && !QFileInfo(companionPath(category, fileName, companionFileName)).isFile()) continue;
        if (entry.toObject().contains("companionFiles") && !entry.toObject().value("companionFiles").isObject()) continue;
        auto files = entry.toObject().value("companionFiles").toObject();
        if (!companionFileName.isEmpty()) files.insert(companionRole(category), companionFileName);
        bool valid = true;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto roles = category == ModelCategory::ImageGeneration || category == ModelCategory::Edition
                ? QStringList{"vae", "clip-l", "clip-g"} : QStringList{companionRole(category)};
            if (!roles.contains(it.key()) || !it.value().isString()) { valid = false; break; }
            const auto path = companionPath(category, fileName, it.value().toString());
            if (path.isEmpty() || !QFileInfo(path).isFile() || !QFileInfo(path).isReadable()) { valid = false; break; }
        }
        const auto type = entry.toObject().value("modelType").toString("checkpoint");
        if (!valid || (type != "checkpoint" && type != "diffusion")) continue;
        models.append({categoryName(category) + QLatin1Char(':') + fileName,
                       fileInfo.completeBaseName(), fileName, fileInfo.size(), fileInfo.absoluteFilePath(),
                       companionFileName, companionFileName.isEmpty() ? QString() : companionRole(category),
                       entry.toObject().value("recommendedOptions").toObject(), files, type});
    }
    std::sort(models.begin(), models.end(), [](const ModelInfo &left, const ModelInfo &right) {
        return QString::compare(left.fileName, right.fileName, Qt::CaseInsensitive) < 0;
    });
    return models;
}

std::optional<ModelInfo> ModelCatalog::model(ModelCategory category, const QString &id) const
{
    const QList<ModelInfo> availableModels = models(category);
    const auto model = std::find_if(availableModels.cbegin(), availableModels.cend(), [&id](const ModelInfo &candidate) {
        return candidate.id == id;
    });
    return model == availableModels.cend() ? std::nullopt : std::optional<ModelInfo>(*model);
}

std::optional<ModelInfo> ModelCatalog::modelById(const QString &id) const
{
    const qsizetype separator = id.indexOf(QLatin1Char(':'));
    if (separator <= 0 || separator == id.size() - 1) return std::nullopt;
    const std::optional<ModelCategory> category = categoryFromName(id.left(separator));
    if (!category.has_value()) return std::nullopt;
    return model(*category, id);
}

bool ModelCatalog::remove(const QString &id, QString *errorMessage)
{
    const std::optional<ModelInfo> modelInfo = modelById(id);
    if (!modelInfo.has_value()) {
        if (errorMessage) *errorMessage = tr("The requested model is not installed");
        return false;
    }
    if (m_activeInstall && m_activeInstall->finalPath == modelInfo->filePath) {
        if (errorMessage) *errorMessage = tr("Companion files are currently being installed for this model");
        return false;
    }
    const ModelCategory category = *categoryFromName(id.left(id.indexOf(QLatin1Char(':'))));
    auto metadata = readMetadata(category);
    if (!metadata) { if (errorMessage) *errorMessage = tr("Could not read model metadata"); return false; }
    const auto previous = *metadata;
    metadata->remove(modelInfo->fileName);
    if (!writeMetadata(category, *metadata)) { if (errorMessage) *errorMessage = tr("Could not update model metadata"); return false; }
    if (!QFile::remove(modelInfo->filePath)) {
        writeMetadata(category, previous);
        if (errorMessage) *errorMessage = tr("Could not remove the model");
        return false;
    }
    for (const auto &name : modelInfo->companionFiles)
        QFile::remove(companionPath(category, modelInfo->fileName, name.toString()));
    return true;
}

bool ModelCatalog::install(const ModelInstallRequest &request, QString *errorMessage)
{
    const auto fail = [&](const QString &message) { if (errorMessage) *errorMessage = message; return false; };
    if (isInstalling()) return fail(tr("Another model installation is in progress"));
    if (request.installId.isEmpty()) return fail(tr("An installation ID is required"));
    if (request.modelType != "checkpoint" && request.modelType != "diffusion"
        && !(request.modelType.isEmpty() && !request.existingModelId.isEmpty())) return fail(tr("Invalid model type"));
    const bool image = request.category == ModelCategory::ImageGeneration || request.category == ModelCategory::Edition;
    if (!image && (request.modelType != "checkpoint" || !request.companionUrls.isEmpty()))
        return fail(tr("CLIP/VAE files and diffusion models require an image model category"));
    if (request.recommendedOptions && !KiraService::Protocol::validateModelOptions(categoryName(request.category), *request.recommendedOptions, errorMessage)) return false;
    const auto metadata = readMetadata(request.category);
    if (!metadata) return fail(tr("Could not read model metadata"));
    auto job = std::make_unique<InstallJob>();
    job->request = request;
    if (!request.existingModelId.isEmpty()) {
        const auto existing = model(request.category, request.existingModelId);
        if (!existing) return fail(tr("The requested model is not installed"));
        job->fileName = existing->fileName;
        job->finalPath = existing->filePath;
        job->companionFiles = existing->companionFiles;
        job->legacyCompanion = existing->companionFileName;
        if (request.modelType.isEmpty()) job->request.modelType = existing->modelType;
    } else {
        const auto url = normalizedModelUrl(request.url);
        if (!acceptsUrl(url)) return fail(tr("Model URLs must be HTTPS URLs to supported model files"));
        job->fileName = fileNameForUrl(url);
        if (!acceptsFileName(request.category, job->fileName)) return fail(tr("The URL filename is not supported for this model category"));
        job->finalPath = QDir(directoryPath(request.category)).filePath(job->fileName);
        if (QFileInfo::exists(job->finalPath)) return fail(tr("A model with this filename is already installed"));
        job->files.append({"primary", url, job->fileName, job->finalPath, job->finalPath + ".part"});
    }
    QJsonObject urls = request.companionUrls;
    const auto legacyRole = companionRole(request.category);
    if (request.companionUrl) {
        if (legacyRole.isEmpty() || urls.contains(legacyRole)) return fail(tr("Invalid or duplicate companion role"));
        urls.insert(legacyRole, request.companionUrl->toString());
    }
    for (auto it = urls.begin(); it != urls.end(); ++it) {
        const auto role = it.key();
        if ((!image && role != legacyRole) || (image && !QStringList{"vae", "clip-l", "clip-g"}.contains(role)) || !it.value().isString())
            return fail(tr("Invalid companion role or URL"));
        if (job->companionFiles.contains(role)) return fail(tr("A file for this companion role is already installed"));
        const auto url = normalizedModelUrl(QUrl(it.value().toString()));
        if (!acceptsUrl(url)) return fail(tr("The companion file URL must be an HTTPS URL to a supported model file"));
        const auto name = fileNameForUrl(url);
        const auto extensions = request.category == ModelCategory::LlmChat ? QStringList{"gguf"} : QStringList{"safetensors", "ckpt", "gguf"};
        if (!isSafeFileName(name) || !extensions.contains(QFileInfo(name).suffix(), Qt::CaseInsensitive)) return fail(tr("The companion filename is not supported"));
        const bool legacy = request.companionUrl && role == legacyRole;
        const auto relative = legacy ? QString("companions/%1/%2").arg(job->fileName, name)
                                    : QString("companions/%1/%2/%3").arg(job->fileName, role, name);
        const auto finalPath = companionPath(request.category, job->fileName, relative);
        if (finalPath.isEmpty() || QFileInfo::exists(finalPath)) return fail(tr("The companion file is already installed for this model"));
        job->companionFiles.insert(role, relative);
        if (legacy) job->legacyCompanion = relative;
        job->files.append({legacy ? QString("companion") : role, url, relative, finalPath, finalPath + ".part"});
    }
    if (job->files.isEmpty() && request.existingModelId.isEmpty()) return fail(tr("No model files were provided"));
    m_activeInstall = std::move(job);
    QTimer::singleShot(0, this, [this, id = request.installId] {
        if (m_activeInstall && m_activeInstall->request.installId == id) startInstall();
    });
    return true;
}

bool ModelCatalog::isInstalling() const { return bool(m_activeInstall); }

void ModelCatalog::cancel(const QString &installId)
{
    if (!m_activeInstall || m_activeInstall->request.installId != installId) return;
    // Reuse cleanup without emitting a failure for an intentional cancellation.
    QSignalBlocker blocker(this);
    failActiveInstall(tr("Model installation canceled"));
    blocker.unblock();
    emit installCanceled(installId);
}

bool ModelCatalog::configure(const QString &id, const QJsonObject &options, QString *error)
{
    const auto info = modelById(id);
    if (!info) { if (error) *error = tr("The requested model is not installed"); return false; }
    const auto category = *categoryFromName(id.section(':', 0, 0));
    if (!KiraService::Protocol::validateModelOptions(categoryName(category), options, error)) return false;
    auto metadata = readMetadata(category);
    if (!metadata) { if (error) *error = tr("Could not read model metadata"); return false; }
    QJsonObject entry = metadata->value(info->fileName).toObject();
    if (!info->companionFileName.isEmpty()) entry.insert("companionFileName", info->companionFileName);
    entry.insert("recommendedOptions", options);
    metadata->insert(info->fileName, entry);
    if (!writeMetadata(category, *metadata)) { if (error) *error = tr("Could not save model metadata"); return false; }
    return true;
}

QString ModelCatalog::rootPath() const
{
    return m_rootPathOverride.has_value() ? *m_rootPathOverride : ServicePaths::modelsPath();
}

QString ModelCatalog::companionMetadataPath(ModelCategory category) const
{
    return QDir(directoryPath(category)).filePath(QStringLiteral("metadata.json"));
}

QString ModelCatalog::companionPath(ModelCategory category, const QString &primaryFileName, const QString &companionFileName) const
{
    const QString prefix = QStringLiteral("companions/%1/").arg(primaryFileName);
    if (!isSafeFileName(primaryFileName) || !companionFileName.startsWith(prefix)) return {};
    const auto parts = companionFileName.mid(prefix.size()).split('/');
    if (parts.size() < 1 || parts.size() > 2) return {};
    for (const auto &part : parts) if (!isSafeFileName(part)) return {};
    if (parts.size() == 2 && !QStringList{"vae", "clip-l", "clip-g", "mmproj", "lora"}.contains(parts.first())) return {};
    return QDir(directoryPath(category)).filePath(companionFileName);
}

std::optional<QJsonObject> ModelCatalog::readMetadata(ModelCategory category) const
{
    QFile file(companionMetadataPath(category));
    if (!QFileInfo::exists(file.fileName())) return QJsonObject{};
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return std::nullopt;
    return document.object();
}

bool ModelCatalog::writeMetadata(ModelCategory category, const QJsonObject &metadata) const
{
    QSaveFile file(companionMetadataPath(category));
    if (!file.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size() && file.commit();
}

QString ModelCatalog::directoryPath(ModelCategory category) const
{
    const QString root = rootPath();
    if (root.isEmpty()) return {};
    switch (category) {
    case ModelCategory::ImageGeneration: return root;
    case ModelCategory::Edition: return QDir(root).filePath(QStringLiteral("edition"));
    case ModelCategory::Upscaler: return QDir(root).filePath(QStringLiteral("upscale"));
    case ModelCategory::LlmChat: return QDir(root).filePath(QStringLiteral("llm"));
    case ModelCategory::IpAdapter: return QDir(root).filePath(QStringLiteral("ip-adapter"));
    }
    return {};
}

bool ModelCatalog::ensureDirectory(ModelCategory category, QString *errorMessage) const
{
    const QString directory = directoryPath(category);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        if (errorMessage) *errorMessage = tr("Could not create the model directory");
        return false;
    }
    return true;
}

bool ModelCatalog::acceptsUrl(const QUrl &url) const
{
    if (!url.isValid() || url.userInfo().size() > 0 || url.host().isEmpty() || url.hasFragment()) return false;
    const bool allowedScheme = url.scheme() == QStringLiteral("https")
                               || (m_allowInsecureTestUrls && url.scheme() == QStringLiteral("http"));
    const bool allowedHost = url.scheme() == QStringLiteral("https")
                             || (m_allowInsecureTestUrls && url.host() == QStringLiteral("127.0.0.1"));
    const QStringList path = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return allowedScheme && allowedHost && !path.isEmpty() && isSafeFileName(path.constLast());
}

bool ModelCatalog::acceptsRedirectUrl(const QUrl &url) const
{
    if (!url.isValid() || !url.userInfo().isEmpty()) return false;
    if (m_allowInsecureTestUrls && url.scheme() == QStringLiteral("http") && url.host() == QStringLiteral("127.0.0.1")) return true;
    return url.scheme() == QStringLiteral("https") && !url.host().isEmpty() && !url.hasFragment();
}

bool ModelCatalog::acceptsFileName(ModelCategory category, const QString &fileName) const
{
    return isSafeFileName(fileName) && extensionsFor(category).contains(QFileInfo(fileName).suffix(), Qt::CaseInsensitive);
}

QString ModelCatalog::fileNameForUrl(const QUrl &url) const
{
    return url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts).constLast();
}

void ModelCatalog::startInstall()
{
    if (!m_activeInstall) return;
    QString error;
    if (!ensureDirectory(m_activeInstall->request.category, &error)) { failActiveInstall(error); return; }
    if (m_activeInstall->fileIndex >= m_activeInstall->files.size()) { completeInstall(); return; }
    const auto &file = m_activeInstall->files.at(m_activeInstall->fileIndex);
    if (!QDir().mkpath(QFileInfo(file.finalPath).absolutePath()) || QFileInfo::exists(file.finalPath)) {
        failActiveInstall(tr("Could not prepare storage without overwriting an installed file")); return;
    }
    QFile::remove(file.partPath);
    m_activeInstall->partFile = std::make_unique<QFile>(file.partPath);
    if (!m_activeInstall->partFile->open(QIODevice::WriteOnly)) { failActiveInstall(tr("Could not prepare model storage")); return; }
    m_activeInstall->redirectCount = 0;
    m_activeInstall->bytesDownloaded = 0;
    m_activeInstall->totalBytes.reset();
    startRequest(file.url);
}

void ModelCatalog::startRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_networkManager->get(request);
    m_activeInstall->reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (!m_activeInstall || m_activeInstall->reply != reply) return;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode >= 300 && statusCode < 400) {
            reply->readAll();
            return;
        }
        writeAvailableData();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (!m_activeInstall || m_activeInstall->reply != reply) return;
        m_activeInstall->bytesDownloaded = received;
        m_activeInstall->totalBytes = total > 0 ? std::optional<qint64>(total) : std::nullopt;
        emitProgress(ModelInstallState::Downloading, received, m_activeInstall->totalBytes);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_activeInstall && m_activeInstall->reply == reply) finishNetworkRequest();
    });
    emitProgress(ModelInstallState::Downloading);
}

void ModelCatalog::finishNetworkRequest()
{
    QNetworkReply *reply = m_activeInstall->reply;
    const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (!redirect.isEmpty()) {
        const QUrl target = reply->url().resolved(redirect);
        reply->deleteLater();
        m_activeInstall->reply = nullptr;
        if (++m_activeInstall->redirectCount > MaximumRedirects || !acceptsRedirectUrl(target)) {
            failActiveInstall(tr("Model download redirected to an untrusted host"));
            return;
        }
        startRequest(target);
        return;
    }
    writeAvailableData();
    if (!m_activeInstall) return;
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
        const auto &file = m_activeInstall->files.at(m_activeInstall->fileIndex);
        const QString fileName = QFileInfo(file.relativeName).fileName();
        const QString role = file.role == "primary" ? tr("model")
            : file.role == "companion" ? companionRole(m_activeInstall->request.category) : file.role;
        const QString reason = statusCode > 0
            ? tr("HTTP %1: %2").arg(statusCode).arg(reply->errorString()) : reply->errorString();
        const QString error = tr("Could not download %1 %2: %3").arg(role, fileName, reason);
        qWarning().noquote() << QStringLiteral("[Kira Service]") << error;
        failActiveInstall(error);
        return;
    }
    m_activeInstall->reply = nullptr;
    reply->deleteLater();
    m_activeInstall->partFile->close();
    ++m_activeInstall->fileIndex;
    if (m_activeInstall->fileIndex < m_activeInstall->files.size()) startInstall();
    else completeInstall();
}

void ModelCatalog::writeAvailableData()
{
    const QByteArray data = m_activeInstall->reply->readAll();
    if (!data.isEmpty() && m_activeInstall->partFile->write(data) != data.size()) {
        failActiveInstall(tr("Could not save the model download"));
    }
}

void ModelCatalog::completeInstall()
{
    if (!m_activeInstall) return;
    const auto category = m_activeInstall->request.category;
    auto metadata = readMetadata(category);
    if (!metadata) { failActiveInstall(tr("Could not read model metadata")); return; }
    if (!m_activeInstall->request.existingModelId.isEmpty() && !QFileInfo(m_activeInstall->finalPath).isFile()) {
        failActiveInstall(tr("The requested model is no longer installed")); return;
    }
    for (auto &file : m_activeInstall->files) {
        if (::link(QFile::encodeName(file.partPath).constData(), QFile::encodeName(file.finalPath).constData()) != 0) {
            failActiveInstall(errno == EEXIST ? tr("A model file is already installed") : tr("Could not install a model file")); return;
        }
        file.published = true;
    }
    auto entry = metadata->value(m_activeInstall->fileName).toObject();
    if (!m_activeInstall->legacyCompanion.isEmpty()) entry.insert("companionFileName", m_activeInstall->legacyCompanion);
    entry.insert("companionFiles", m_activeInstall->companionFiles);
    entry.insert("modelType", m_activeInstall->request.modelType);
    if (m_activeInstall->request.recommendedOptions) entry.insert("recommendedOptions", *m_activeInstall->request.recommendedOptions);
    metadata->insert(m_activeInstall->fileName, entry);
    if (!writeMetadata(category, *metadata)) { failActiveInstall(tr("Could not save model metadata")); return; }
    for (const auto &file : m_activeInstall->files) QFile::remove(file.partPath);
    const auto id = categoryName(category) + ':' + m_activeInstall->fileName;
    const auto installed = model(category, id);
    const auto installId = m_activeInstall->request.installId;
    m_activeInstall.reset();
    if (installed) emit installFinished(installId, *installed);
    else emit installFailed(installId, tr("Could not read the installed model"));
}

void ModelCatalog::failActiveInstall(const QString &errorMessage)
{
    if (!m_activeInstall) return;
    const QString installId = m_activeInstall->request.installId;
    QNetworkReply *reply = m_activeInstall->reply.data();
    m_activeInstall->reply = nullptr;
    if (reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    if (m_activeInstall->partFile) m_activeInstall->partFile->close();
    for (const auto &file : m_activeInstall->files) {
        QFile::remove(file.partPath);
        if (file.published) QFile::remove(file.finalPath);
    }
    m_activeInstall.reset();
    emit installFailed(installId, errorMessage);
}

void ModelCatalog::emitProgress(ModelInstallState state, qint64 bytesDownloaded, std::optional<qint64> totalBytes)
{
    if (m_activeInstall && m_activeInstall->fileIndex < m_activeInstall->files.size())
        emit installProgress({m_activeInstall->request.installId, state, bytesDownloaded, totalBytes,
            m_activeInstall->files.at(m_activeInstall->fileIndex).role});
}
