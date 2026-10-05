#pragma once

#include <QList>
#include <QTcpServer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <optional>
#include <kira/service/PromptStyle.h>

class QWebSocket;

struct ManagedModelInfo
{
    QString id;
    QString name;
    QString fileName;
    qint64 fileSize = 0;
    QString companionFileName;
    QString companionRole;
    QJsonObject recommendedOptions;
    QJsonObject companionFiles;
    QString modelType = QStringLiteral("checkpoint");
    bool explicitContent = false;
    QList<KiraService::PromptStyle> styles;
    QString displayName;
    QString displayLabel() const { return displayName.trimmed().isEmpty() ? fileName : displayName.trimmed(); }
};

enum class ManagedInstallState { Requesting, Accepted, Queued, Downloading, Verifying, Completed, Failed, Canceled };

struct ManagedInstall
{
    QString installId;
    QString category;
    QUrl url;
    QUrl companionUrl;
    QString companionFileName;
    ManagedInstallState state = ManagedInstallState::Requesting;
    qint64 bytesDownloaded = 0;
    std::optional<qint64> totalBytes;
    QString message;
    QString modelId;
    QString fileRole;
};

class ManagerServiceClient final : public QObject
{
    Q_OBJECT

public:
    explicit ManagerServiceClient(QObject *parent = nullptr, quint16 localPort = 9384);

    void connectToService();
    bool setConnection(const QString &host, const QString &token);
    QString host() const;
    QString token() const;
    void setAutomaticReconnect(bool enabled);
    bool isAuthenticated() const;
    bool modelListAvailable() const;
    QList<ManagedModelInfo> models(const QString &category) const;
    QList<ManagedInstall> installs() const;
    void requestModels();
    QString installModel(const QString &category, const QUrl &url, const QUrl &companionUrl = {}, const QJsonObject &recommendedOptions = {}, const QJsonObject &companionUrls = {},
        const QString &modelType = QStringLiteral("checkpoint"), const QString &existingModelId = {}, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {});
    bool configureModel(const QString &id, const QJsonObject &options, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {});
    bool configureModelStyles(const QString &id, const QList<KiraService::PromptStyle> &styles);
    bool cancelInstall(const QString &installId);
    bool deleteModel(const QString &id);
    bool deleteCompanion(const QString& id, const QString& role);

signals:
    void serviceStateChanged(const QString &state, const QString &message);
    void modelsChanged();
    void installsChanged();
    void modelConfigured(const QString &id, const QJsonObject &options, bool explicitContent);
    void modelStylesConfigured(const QString &id, const QList<KiraService::PromptStyle> &styles);
    void modelDeleted(const QString &id);
    void companionDeleted(const QString& id, const QString& role);
    void modelOperationFailed(const QString &id, const QString &message);

private:
    QTcpServer m_localSourcesServer;
    QHash<QString, QString> m_localSources;
    QUrl prepareSource(const QUrl& source);
    void authenticate();
    void handleTextMessage(const QString &message);
    ManagedInstall *findInstall(const QString &installId);
    void failActiveInstalls(const QString &message);

    const quint16 m_localPort;
    QString m_host = QStringLiteral("127.0.0.1");
    QString m_token;
    QTimer m_connectionTimeout;
    QWebSocket *m_socket;
    QTimer *m_retryTimer;
    bool m_automaticReconnect = true;
    bool m_openingConnection = false;
    bool m_authenticated = false;
    bool m_modelListAvailable = false;
    QList<ManagedModelInfo> m_generationModels;
    QList<ManagedModelInfo> m_editionModels;
    QList<ManagedModelInfo> m_upscaleModels;
    QList<ManagedModelInfo> m_llmChatModels;
    QList<ManagedModelInfo> m_promptEnhancerModels;
    QList<ManagedModelInfo> m_ipAdapterModels;
    QList<ManagedModelInfo> m_clipVisionModels;
    QList<ManagedModelInfo> m_faceParsingModels;
    QList<ManagedModelInfo> m_inpaintModels;
    QList<ManagedModelInfo> m_photoMakerModels;
    QList<ManagedInstall> m_installs;
};
