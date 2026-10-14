#pragma once

#include <QList>
#include <QHash>
#include <QObject>
#include <QUrl>
#include <kira/service/PromptStyle.h>
#include <kira/service/ModelRouting.h>

#include <optional>
#include <QJsonObject>

enum class ModelCategory
{
    ImageGeneration,
    Upscaler,
    LlmChat,
    Companion,
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
    QJsonObject recommendedOptions;
    QJsonObject companionFiles;
    QString modelType = QStringLiteral("checkpoint");
    bool explicitContent = false;
    QList<KiraService::PromptStyle> styles;
    QString displayName;
    QString mmprojModelId;
};

Q_DECLARE_METATYPE(ModelInfo)

struct ModelInstallRequest
{
    QString installId;
    ModelCategory category = ModelCategory::ImageGeneration;
    QUrl url;
    std::optional<QUrl> companionUrl;
    std::optional<QJsonObject> recommendedOptions = {};
    QJsonObject companionUrls;
    QString existingModelId;
    QString modelType = QStringLiteral("checkpoint");
    std::optional<bool> explicitContent;
    bool allowLocalFiles = false;
    std::optional<QString> displayName;
    std::optional<QString> mmprojModelId;
};

enum class ModelInstallState
{
    Downloading,
    Verifying
};

struct ModelInstallProgress
{
    QString installId;
    ModelInstallState state = ModelInstallState::Downloading;
    qint64 bytesDownloaded = 0;
    std::optional<qint64> totalBytes;
    QString fileRole = QStringLiteral("primary");
};

class ModelCatalog final : public QObject
{
    Q_OBJECT

public:
    explicit ModelCatalog(QObject *parent = nullptr);
    ModelCatalog(QString rootPathOverride, bool allowInsecureTestUrls, QObject *parent = nullptr);
    // Split layout: weights under modelsPath/<category>/, catalogue JSON files in catalogPath.
    ModelCatalog(QString modelsPath, QString catalogPath, bool allowInsecureTestUrls, QObject *parent = nullptr);
    ~ModelCatalog() override;

    static QList<ModelCategory> categories();
    static QString categoryName(ModelCategory category);
    static QString companionRole(ModelCategory category);
    static std::optional<ModelCategory> categoryFromName(const QString &name);
    bool ensureStorage(QString *errorMessage = nullptr) const;
    QList<ModelInfo> models(ModelCategory category) const;
    std::optional<ModelInfo> model(ModelCategory category, const QString &id) const;
    std::optional<ModelInfo> modelById(const QString &id) const;
    bool install(const ModelInstallRequest &request, QString *errorMessage = nullptr);
    bool remove(const QString &id, QString *errorMessage = nullptr);
    bool removeCompanion(const QString& id, const QString& role, QString* error = nullptr);
    void cancel(const QString &installId);
    bool isInstalling() const;
    bool configure(const QString &id, const QJsonObject &options, QString *error = nullptr, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {}, std::optional<QString> mmprojModelId = {});
    bool configureStyles(const QString &id, const QList<KiraService::PromptStyle> &styles, QString *error = nullptr);
    std::optional<KiraService::ModelRouting> routing(QString *error = nullptr) const;
    bool configureRouting(const KiraService::ModelRouting &routing, QString *error = nullptr);

signals:
    void installProgress(const ModelInstallProgress &progress);
    void installFinished(const QString &installId, const ModelInfo &model);
    void installCanceled(const QString &installId);
    void installFailed(const QString &installId, const QString &errorMessage);

private:
    struct InstallJob;
    bool validateMmproj(ModelCategory category, const std::optional<QString>& id, QString* error) const;
    bool validateImageReferences(ModelCategory category, const QJsonObject& options, QString* error) const;
    bool isImageReference(const QString& id) const;
    bool validateRoutingReferences(const KiraService::ModelRouting &routing, QString *error) const;
    void initializeRoutingOptions(KiraService::ModelRouting &routing) const;
    bool protectRouting(const QString &id, const QJsonObject *options, QString *error) const;

    QString rootPath() const;
    QString configurationPath() const;
    QString directoryPath(ModelCategory category) const;
    QString companionMetadataPath(ModelCategory category) const;
    QString companionPath(ModelCategory category, const QString &primaryFileName, const QString &companionFileName) const;
    std::optional<QJsonObject> readMetadata(ModelCategory category) const;
    bool writeMetadata(ModelCategory category, const QJsonObject &metadata) const;
    bool ensureDirectory(ModelCategory category, QString *errorMessage) const;
    bool acceptsUrl(const QUrl &url) const;
    bool acceptsRedirectUrl(const QUrl &url) const;
    bool acceptsFileName(ModelCategory category, const QString &fileName) const;
    QString fileNameForUrl(const QUrl &url) const;
    void startInstall();
    void startRequest(const QUrl &url);
    void finishNetworkRequest();
    void writeAvailableData();
    void completeInstall();
    void failActiveInstall(const QString &errorMessage);
    void emitProgress(ModelInstallState state, qint64 bytesDownloaded = 0, std::optional<qint64> totalBytes = {});

    std::optional<QString> m_rootPathOverride;
    std::optional<QString> m_catalogPath;
    bool m_allowInsecureTestUrls = false;
    class QNetworkAccessManager *m_networkManager;
    std::unique_ptr<InstallJob> m_activeInstall;
};

Q_DECLARE_METATYPE(ModelInstallProgress)
