#pragma once

#include <QList>
#include <QHash>
#include <QObject>
#include <QUrl>

#include <optional>
#include <deque>

enum class ModelCategory
{
    ImageGeneration,
    Upscaler,
    LlmChat,
    IpAdapter,
    Edition
};

struct ModelInfo
{
    QString id;
    QString name;
    QString fileName;
    qint64 fileSize = 0;
    QString filePath;
    QString companionFileName;
    QString companionRole;
};

Q_DECLARE_METATYPE(ModelInfo)

struct ModelInstallRequest
{
    QString installId;
    ModelCategory category = ModelCategory::ImageGeneration;
    QUrl url;
    std::optional<QUrl> companionUrl;
};

enum class ModelInstallState
{
    Queued,
    Downloading,
    Verifying
};

struct ModelInstallProgress
{
    QString installId;
    ModelInstallState state = ModelInstallState::Queued;
    qint64 bytesDownloaded = 0;
    std::optional<qint64> totalBytes;
};

class ModelCatalog final : public QObject
{
    Q_OBJECT

public:
    explicit ModelCatalog(QObject *parent = nullptr);
    ModelCatalog(QString rootPathOverride, bool allowInsecureTestUrls, QObject *parent = nullptr);
    ~ModelCatalog() override;

    static QList<ModelCategory> categories();
    static QString categoryName(ModelCategory category);
    static QString companionRole(ModelCategory category);
    static std::optional<ModelCategory> categoryFromName(const QString &name);
    QList<ModelInfo> models(ModelCategory category) const;
    std::optional<ModelInfo> model(ModelCategory category, const QString &id) const;
    std::optional<ModelInfo> modelById(const QString &id) const;
    bool install(const ModelInstallRequest &request, QString *errorMessage = nullptr);
    bool remove(const QString &id, QString *errorMessage = nullptr);
    void cancel(const QString &installId);

signals:
    void installProgress(const ModelInstallProgress &progress);
    void installFinished(const QString &installId, const ModelInfo &model);
    void installFailed(const QString &installId, const QString &errorMessage);

private:
    struct InstallJob;

    QString rootPath() const;
    QString directoryPath(ModelCategory category) const;
    QString companionMetadataPath(ModelCategory category) const;
    QString companionPath(ModelCategory category, const QString &primaryFileName, const QString &companionFileName) const;
    QHash<QString, QString> companionMetadata(ModelCategory category) const;
    bool writeCompanionMetadata(ModelCategory category, const QHash<QString, QString> &metadata) const;
    bool ensureDirectory(ModelCategory category, QString *errorMessage) const;
    bool acceptsUrl(const QUrl &url) const;
    bool acceptsRedirectUrl(const QUrl &url) const;
    bool acceptsFileName(ModelCategory category, const QString &fileName) const;
    QString fileNameForUrl(const QUrl &url) const;
    void startNextInstall();
    void startRequest(const QUrl &url);
    void startCompanionInstall();
    void finishNetworkRequest();
    void writeAvailableData();
    void completeInstall();
    void failActiveInstall(const QString &errorMessage);
    void emitProgress(ModelInstallState state, qint64 bytesDownloaded = 0, std::optional<qint64> totalBytes = {});

    std::optional<QString> m_rootPathOverride;
    bool m_allowInsecureTestUrls = false;
    class QNetworkAccessManager *m_networkManager;
    std::deque<std::unique_ptr<InstallJob>> m_pendingInstalls;
    std::unique_ptr<InstallJob> m_activeInstall;
};

Q_DECLARE_METATYPE(ModelInstallProgress)
