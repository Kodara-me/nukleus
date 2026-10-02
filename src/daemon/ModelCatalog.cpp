#include "ModelCatalog.h"

#include "ServicePaths.h"

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
    QString partPath;
    QString companionFileName;
    QString companionFinalPath;
    QString companionPartPath;
    bool downloadingCompanion = false;
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
    const QHash<QString, QString> metadata = companionMetadata(category);
    for (const QFileInfo &fileInfo : directory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name)) {
        if (!acceptsFileName(category, fileInfo.fileName())) continue;
        const QString fileName = fileInfo.fileName();
        const QString companionFileName = metadata.value(fileName);
        if (!companionFileName.isEmpty() && !QFileInfo(companionPath(category, fileName, companionFileName)).isFile()) continue;
        models.append({categoryName(category) + QLatin1Char(':') + fileName,
                       fileInfo.completeBaseName(), fileName, fileInfo.size(), fileInfo.absoluteFilePath(), companionFileName, companionFileName.isEmpty() ? QString() : companionRole(category)});
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
    if (!QFile::remove(modelInfo->filePath)) {
        if (errorMessage) *errorMessage = tr("Could not remove the model");
        return false;
    }
    const ModelCategory category = *categoryFromName(id.left(id.indexOf(QLatin1Char(':'))));
    if (!modelInfo->companionFileName.isEmpty()) {
        QFile::remove(companionPath(category, QFileInfo(modelInfo->fileName).fileName(), modelInfo->companionFileName));
        QHash<QString, QString> metadata = companionMetadata(category);
        metadata.remove(modelInfo->fileName);
        if (!writeCompanionMetadata(category, metadata)) {
            if (errorMessage) *errorMessage = tr("Could not update companion metadata");
            return false;
        }
    }
    return true;
}

bool ModelCatalog::install(const ModelInstallRequest &request, QString *errorMessage)
{
    if (request.installId.isEmpty()) {
        if (errorMessage) *errorMessage = tr("An installation ID is required");
        return false;
    }
    const QUrl downloadUrl = normalizedModelUrl(request.url);
    if (!acceptsUrl(downloadUrl)) {
        if (errorMessage) *errorMessage = tr("Model URLs must be HTTPS URLs to supported model files");
        return false;
    }
    const QString fileName = fileNameForUrl(downloadUrl);
    if (!acceptsFileName(request.category, fileName)) {
        if (errorMessage) *errorMessage = tr("The URL filename is not supported for this model category");
        return false;
    }
    std::optional<QUrl> companionUrl;
    QString companionFileName;
    QString companionRelativePath;
    if (request.companionUrl.has_value()) {
        if (companionRole(request.category).isEmpty()) {
            if (errorMessage) *errorMessage = tr("This model category does not support a companion file");
            return false;
        }
        const QUrl normalizedCompanionUrl = normalizedModelUrl(*request.companionUrl);
        if (!acceptsUrl(normalizedCompanionUrl)) {
            if (errorMessage) *errorMessage = tr("The companion file URL must be an HTTPS URL to a supported model file");
            return false;
        }
        companionFileName = fileNameForUrl(normalizedCompanionUrl);
        if (!isSafeFileName(companionFileName)
            || !(request.category == ModelCategory::LlmChat ? QStringList{QStringLiteral("gguf")}
                : QStringList{QStringLiteral("safetensors"), QStringLiteral("ckpt"), QStringLiteral("gguf")})
                    .contains(QFileInfo(companionFileName).suffix(), Qt::CaseInsensitive)) {
            if (errorMessage) *errorMessage = tr("The companion filename is not supported");
            return false;
        }
        companionUrl = normalizedCompanionUrl;
        companionRelativePath = QStringLiteral("companions/%1/%2").arg(fileName, companionFileName);
    }
    const QString finalPath = QDir(directoryPath(request.category)).filePath(fileName);
    const QString companionFinalPath = companionRelativePath.isEmpty() ? QString() : companionPath(request.category, fileName, companionRelativePath);
    for (const auto &pending : m_pendingInstalls) {
        if (pending->request.installId == request.installId || pending->finalPath == finalPath) {
            if (errorMessage) *errorMessage = tr("A matching model installation is already queued");
            return false;
        }
    }
    if (m_activeInstall && (m_activeInstall->request.installId == request.installId
                            || (m_activeInstall->fileName == fileName && m_activeInstall->request.category == request.category))) {
        if (errorMessage) *errorMessage = tr("A matching model installation is already in progress");
        return false;
    }
    if (QFileInfo::exists(finalPath)) {
        if (errorMessage) *errorMessage = tr("A model with this filename is already installed");
        return false;
    }
    if (!companionFinalPath.isEmpty() && QFileInfo::exists(companionFinalPath)) {
        if (errorMessage) *errorMessage = tr("The companion file is already installed for this model");
        return false;
    }
    auto job = std::make_unique<InstallJob>();
    job->request = request;
    job->request.url = downloadUrl;
    job->request.companionUrl = companionUrl;
    job->fileName = fileName;
    job->finalPath = finalPath;
    job->partPath = finalPath + QStringLiteral(".part");
    job->companionFileName = companionRelativePath;
    job->companionFinalPath = companionFinalPath;
    job->companionPartPath = companionFinalPath.isEmpty() ? QString() : companionFinalPath + QStringLiteral(".part");
    m_pendingInstalls.push_back(std::move(job));
    QTimer::singleShot(0, this, [this, installId = request.installId] {
        if (m_activeInstall && m_activeInstall->request.installId == installId) {
            emit installProgress({installId, ModelInstallState::Queued, 0, {}});
        } else {
            for (const auto &pending : m_pendingInstalls) {
                if (pending->request.installId == installId) {
                    emit installProgress({installId, ModelInstallState::Queued, 0, {}});
                    break;
                }
            }
        }
    });
    QTimer::singleShot(0, this, &ModelCatalog::startNextInstall);
    return true;
}

void ModelCatalog::cancel(const QString &installId)
{
    for (auto it = m_pendingInstalls.begin(); it != m_pendingInstalls.end(); ++it) {
        if ((*it)->request.installId == installId) {
            m_pendingInstalls.erase(it);
            emit installFailed(installId, tr("Model installation canceled"));
            return;
        }
    }
    if (m_activeInstall && m_activeInstall->request.installId == installId) {
        failActiveInstall(tr("Model installation canceled"));
    }
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
    if (!isSafeFileName(primaryFileName) || !companionFileName.startsWith(prefix)
        || !isSafeFileName(companionFileName.mid(prefix.size()))) return {};
    return QDir(directoryPath(category)).filePath(companionFileName);
}

QHash<QString, QString> ModelCatalog::companionMetadata(ModelCategory category) const
{
    QHash<QString, QString> metadata;
    QFile file(companionMetadataPath(category));
    if (!file.open(QIODevice::ReadOnly)) return metadata;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return metadata;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isString() || it.key().isEmpty()) continue;
        const QString relativePath = it.value().toString();
        if (companionPath(category, it.key(), relativePath).isEmpty()) continue;
        metadata.insert(it.key(), relativePath);
    }
    return metadata;
}

bool ModelCatalog::writeCompanionMetadata(ModelCategory category, const QHash<QString, QString> &metadata) const
{
    QSaveFile file(companionMetadataPath(category));
    if (!file.open(QIODevice::WriteOnly)) return false;
    QJsonObject object;
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) object.insert(it.key(), it.value());
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0) return false;
    return file.commit();
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

void ModelCatalog::startNextInstall()
{
    if (m_activeInstall || m_pendingInstalls.empty()) return;
    m_activeInstall = std::move(m_pendingInstalls.front());
    m_pendingInstalls.pop_front();
    QString error;
    if (!ensureDirectory(m_activeInstall->request.category, &error)) {
        failActiveInstall(error);
        return;
    }
    if (!m_activeInstall->companionFinalPath.isEmpty()
        && !QDir().mkpath(QFileInfo(m_activeInstall->companionFinalPath).absolutePath())) {
        failActiveInstall(tr("Could not create companion model storage"));
        return;
    }
    if (QFileInfo::exists(m_activeInstall->finalPath)) {
        failActiveInstall(tr("A model with this filename is already installed"));
        return;
    }
    QFile::remove(m_activeInstall->partPath);
    if (!m_activeInstall->companionPartPath.isEmpty()) QFile::remove(m_activeInstall->companionPartPath);
    m_activeInstall->partFile = std::make_unique<QFile>(m_activeInstall->partPath);
    if (!m_activeInstall->partFile->open(QIODevice::WriteOnly)) {
        failActiveInstall(tr("Could not prepare model storage"));
        return;
    }
    startRequest(m_activeInstall->request.url);
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

void ModelCatalog::startCompanionInstall()
{
    if (!m_activeInstall || !m_activeInstall->request.companionUrl.has_value()) return;
    m_activeInstall->downloadingCompanion = true;
    m_activeInstall->bytesDownloaded = 0;
    m_activeInstall->totalBytes.reset();
    m_activeInstall->partFile = std::make_unique<QFile>(m_activeInstall->companionPartPath);
    if (!m_activeInstall->partFile->open(QIODevice::WriteOnly)) {
        failActiveInstall(tr("Could not prepare companion model storage"));
        return;
    }
    startRequest(*m_activeInstall->request.companionUrl);
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
        const QString fileName = m_activeInstall->downloadingCompanion
            ? QFileInfo(m_activeInstall->companionFileName).fileName() : m_activeInstall->fileName;
        const QString role = m_activeInstall->downloadingCompanion
            ? companionRole(m_activeInstall->request.category) : tr("model");
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
    if (m_activeInstall->downloadingCompanion) {
        completeInstall();
        return;
    }
    completeInstall();
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
    const ModelCategory category = m_activeInstall->request.category;
    if (!m_activeInstall->request.companionUrl.has_value() && !m_activeInstall->downloadingCompanion) {
        const QByteArray partPath = QFile::encodeName(m_activeInstall->partPath);
        const QByteArray finalPath = QFile::encodeName(m_activeInstall->finalPath);
        if (::link(partPath.constData(), finalPath.constData()) != 0) {
            failActiveInstall(errno == EEXIST ? tr("A model with this filename is already installed") : tr("Could not install the model"));
            return;
        }
        QFile::remove(m_activeInstall->partPath);
    } else if (!m_activeInstall->downloadingCompanion) {
        startCompanionInstall();
        return;
    } else {
        m_activeInstall->partFile->close();
        const QByteArray companionPartPath = QFile::encodeName(m_activeInstall->companionPartPath);
        const QByteArray companionFinalPath = QFile::encodeName(m_activeInstall->companionFinalPath);
        if (::link(companionPartPath.constData(), companionFinalPath.constData()) != 0) {
            failActiveInstall(errno == EEXIST ? tr("The companion file is already installed") : tr("Could not install the companion file"));
            return;
        }
        const QByteArray partPath = QFile::encodeName(m_activeInstall->partPath);
        const QByteArray finalPath = QFile::encodeName(m_activeInstall->finalPath);
        if (::link(partPath.constData(), finalPath.constData()) != 0) {
            QFile::remove(m_activeInstall->companionFinalPath);
            failActiveInstall(errno == EEXIST ? tr("A model with this filename is already installed") : tr("Could not install the model"));
            return;
        }
        QHash<QString, QString> metadata = companionMetadata(category);
        metadata.insert(m_activeInstall->fileName, m_activeInstall->companionFileName);
        if (!writeCompanionMetadata(category, metadata)) {
            QFile::remove(m_activeInstall->finalPath);
            QFile::remove(m_activeInstall->companionFinalPath);
            failActiveInstall(tr("Could not save companion metadata"));
            return;
        }
        QFile::remove(m_activeInstall->partPath);
        QFile::remove(m_activeInstall->companionPartPath);
    }
    const ModelInfo model{categoryName(m_activeInstall->request.category) + QLatin1Char(':') + m_activeInstall->fileName,
                          QFileInfo(m_activeInstall->fileName).completeBaseName(), m_activeInstall->fileName,
                          QFileInfo(m_activeInstall->finalPath).size(), m_activeInstall->finalPath,
                          m_activeInstall->companionFileName,
                          m_activeInstall->companionFileName.isEmpty() ? QString() : companionRole(category)};
    const QString installId = m_activeInstall->request.installId;
    m_activeInstall.reset();
    emit installFinished(installId, model);
    startNextInstall();
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
    QFile::remove(m_activeInstall->partPath);
    QFile::remove(m_activeInstall->companionPartPath);
    m_activeInstall.reset();
    emit installFailed(installId, errorMessage);
    startNextInstall();
}

void ModelCatalog::emitProgress(ModelInstallState state, qint64 bytesDownloaded, std::optional<qint64> totalBytes)
{
    if (m_activeInstall) emit installProgress({m_activeInstall->request.installId, state, bytesDownloaded, totalBytes});
}
