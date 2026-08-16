#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

#include <optional>
#include <deque>

enum class ModelCategory
{
    ImageGeneration,
    Upscaler,
    LlmChat
};

struct ModelInfo
{
    QString id;
    QString name;
    QString fileName;
    qint64 fileSize = 0;
    QString filePath;
};

Q_DECLARE_METATYPE(ModelInfo)

struct ModelInstallRequest
{
    QString installId;
    ModelCategory category = ModelCategory::ImageGeneration;
    QUrl url;
    std::optional<QString> expectedSha256;
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
    bool ensureDirectory(ModelCategory category, QString *errorMessage) const;
    bool acceptsUrl(const QUrl &url) const;
    bool acceptsRedirectUrl(const QUrl &url) const;
    bool acceptsFileName(ModelCategory category, const QString &fileName) const;
    QString fileNameForUrl(const QUrl &url) const;
    void startNextInstall();
    void startRequest(const QUrl &url);
    void finishNetworkRequest();
    void writeAvailableData();
    void startVerification();
    void verifyNextChunk();
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
