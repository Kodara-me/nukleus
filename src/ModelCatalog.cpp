#include "ModelCatalog.h"

#include "ServicePaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace
{
constexpr qint64 VerificationChunkSize = 1024 * 1024;
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
        return {QStringLiteral("safetensors"), QStringLiteral("ckpt"), QStringLiteral("gguf")};
    case ModelCategory::Upscaler:
    case ModelCategory::LlmChat:
        return {QStringLiteral("gguf")};
    }
    return {};
}
}

struct ModelCatalog::InstallJob
{
    ModelInstallRequest request;
    QString fileName;
    QString finalPath;
    QString partPath;
    std::unique_ptr<QFile> partFile;
    std::unique_ptr<QFile> verificationFile;
    QPointer<QNetworkReply> reply;
    QCryptographicHash sha256{QCryptographicHash::Sha256};
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
    return {ModelCategory::ImageGeneration, ModelCategory::Upscaler, ModelCategory::LlmChat};
}

QString ModelCatalog::categoryName(ModelCategory category)
{
    switch (category) {
    case ModelCategory::ImageGeneration: return QStringLiteral("image-generation");
    case ModelCategory::Upscaler: return QStringLiteral("upscaler");
    case ModelCategory::LlmChat: return QStringLiteral("llm-chat");
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
    for (const QFileInfo &fileInfo : directory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name)) {
        if (!acceptsFileName(category, fileInfo.fileName())) continue;
        const QString fileName = fileInfo.fileName();
        models.append({categoryName(category) + QLatin1Char(':') + fileName,
                       fileInfo.completeBaseName(), fileName, fileInfo.size(), fileInfo.absoluteFilePath()});
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

bool ModelCatalog::install(const ModelInstallRequest &request, QString *errorMessage)
{
    if (request.installId.isEmpty()) {
        if (errorMessage) *errorMessage = tr("An installation ID is required");
        return false;
    }
    if (!acceptsUrl(request.url)) {
        if (errorMessage) *errorMessage = tr("Model URLs must be direct HTTPS Hugging Face resolve URLs");
        return false;
    }
    const QString fileName = fileNameForUrl(request.url);
    if (!acceptsFileName(request.category, fileName)) {
        if (errorMessage) *errorMessage = tr("The URL filename is not supported for this model category");
        return false;
    }
    if (request.expectedSha256.has_value()
        && !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(*request.expectedSha256).hasMatch()) {
        if (errorMessage) *errorMessage = tr("The SHA-256 checksum must contain 64 hexadecimal characters");
        return false;
    }
    const QString finalPath = QDir(directoryPath(request.category)).filePath(fileName);
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
    auto job = std::make_unique<InstallJob>();
    job->request = request;
    job->fileName = fileName;
    job->finalPath = finalPath;
    job->partPath = finalPath + QStringLiteral(".part");
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

QString ModelCatalog::directoryPath(ModelCategory category) const
{
    const QString root = rootPath();
    if (root.isEmpty()) return {};
    switch (category) {
    case ModelCategory::ImageGeneration: return root;
    case ModelCategory::Upscaler: return QDir(root).filePath(QStringLiteral("upscale"));
    case ModelCategory::LlmChat: return QDir(root).filePath(QStringLiteral("llm"));
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
    if (!url.isValid() || url.userInfo().size() > 0) return false;
    const bool allowedScheme = url.scheme() == QStringLiteral("https")
                               || (m_allowInsecureTestUrls && url.scheme() == QStringLiteral("http"));
    const bool allowedHost = url.host().compare(QStringLiteral("huggingface.co"), Qt::CaseInsensitive) == 0
                             || (m_allowInsecureTestUrls && url.host() == QStringLiteral("127.0.0.1"));
    const QStringList path = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return allowedScheme && allowedHost && path.size() >= 5 && path.at(2) == QStringLiteral("resolve")
           && !path.at(0).isEmpty() && !path.at(1).isEmpty() && !path.at(3).isEmpty() && isSafeFileName(path.constLast());
}

bool ModelCatalog::acceptsRedirectUrl(const QUrl &url) const
{
    if (!url.isValid() || !url.userInfo().isEmpty()) return false;
    if (m_allowInsecureTestUrls && url.scheme() == QStringLiteral("http") && url.host() == QStringLiteral("127.0.0.1")) return true;
    const QString host = url.host().toLower();
    return url.scheme() == QStringLiteral("https")
           && (host == QStringLiteral("cdn-lfs.huggingface.co") || host.endsWith(QStringLiteral(".hf.co")));
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
    if (QFileInfo::exists(m_activeInstall->finalPath)) {
        failActiveInstall(tr("A model with this filename is already installed"));
        return;
    }
    QFile::remove(m_activeInstall->partPath);
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
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
        failActiveInstall(tr("Could not download the model"));
        return;
    }
    m_activeInstall->reply = nullptr;
    reply->deleteLater();
    m_activeInstall->partFile->close();
    startVerification();
}

void ModelCatalog::writeAvailableData()
{
    const QByteArray data = m_activeInstall->reply->readAll();
    if (!data.isEmpty() && m_activeInstall->partFile->write(data) != data.size()) {
        failActiveInstall(tr("Could not save the model download"));
    }
}

void ModelCatalog::startVerification()
{
    if (!m_activeInstall) return;
    emitProgress(ModelInstallState::Verifying, m_activeInstall->bytesDownloaded, m_activeInstall->totalBytes);
    if (!m_activeInstall->request.expectedSha256.has_value()) {
        completeInstall();
        return;
    }
    m_activeInstall->verificationFile = std::make_unique<QFile>(m_activeInstall->partPath);
    if (!m_activeInstall->verificationFile->open(QIODevice::ReadOnly)) {
        failActiveInstall(tr("Could not verify the model download"));
        return;
    }
    QTimer::singleShot(0, this, &ModelCatalog::verifyNextChunk);
}

void ModelCatalog::verifyNextChunk()
{
    if (!m_activeInstall || !m_activeInstall->verificationFile) return;
    const QByteArray data = m_activeInstall->verificationFile->read(VerificationChunkSize);
    if (!data.isEmpty()) {
        m_activeInstall->sha256.addData(data);
        QTimer::singleShot(0, this, &ModelCatalog::verifyNextChunk);
        return;
    }
    if (m_activeInstall->verificationFile->error() != QFile::NoError
        || QString::fromLatin1(m_activeInstall->sha256.result().toHex()).compare(*m_activeInstall->request.expectedSha256, Qt::CaseInsensitive) != 0) {
        failActiveInstall(tr("Could not verify the model download"));
        return;
    }
    m_activeInstall->verificationFile->close();
    completeInstall();
}

void ModelCatalog::completeInstall()
{
    if (!m_activeInstall) return;
    const QByteArray partPath = QFile::encodeName(m_activeInstall->partPath);
    const QByteArray finalPath = QFile::encodeName(m_activeInstall->finalPath);
    if (::link(partPath.constData(), finalPath.constData()) != 0) {
        failActiveInstall(errno == EEXIST ? tr("A model with this filename is already installed") : tr("Could not install the model"));
        return;
    }
    QFile::remove(m_activeInstall->partPath);
    const ModelInfo model{categoryName(m_activeInstall->request.category) + QLatin1Char(':') + m_activeInstall->fileName,
                          QFileInfo(m_activeInstall->fileName).completeBaseName(), m_activeInstall->fileName,
                          QFileInfo(m_activeInstall->finalPath).size(), m_activeInstall->finalPath};
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
    if (m_activeInstall->verificationFile) m_activeInstall->verificationFile->close();
    QFile::remove(m_activeInstall->partPath);
    m_activeInstall.reset();
    emit installFailed(installId, errorMessage);
    startNextInstall();
}

void ModelCatalog::emitProgress(ModelInstallState state, qint64 bytesDownloaded, std::optional<qint64> totalBytes)
{
    if (m_activeInstall) emit installProgress({m_activeInstall->request.installId, state, bytesDownloaded, totalBytes});
}
