#pragma once

#include <QList>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <optional>

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
    void setAutomaticReconnect(bool enabled);
    bool isAuthenticated() const;
    bool modelListAvailable() const;
    QList<ManagedModelInfo> models(const QString &category) const;
    QList<ManagedInstall> installs() const;
    void requestModels();
    QString installModel(const QString &category, const QUrl &url, const QUrl &companionUrl = {}, const QJsonObject &recommendedOptions = {});
    bool configureModel(const QString &id, const QJsonObject &options);
    bool cancelInstall(const QString &installId);
    bool deleteModel(const QString &id);

signals:
    void serviceStateChanged(const QString &state, const QString &message);
    void modelsChanged();
    void installsChanged();
    void modelConfigured(const QString &id, const QJsonObject &options);
    void modelDeleted(const QString &id);
    void modelOperationFailed(const QString &id, const QString &message);

private:
    void authenticate();
    void handleTextMessage(const QString &message);
    ManagedInstall *findInstall(const QString &installId);
    void failActiveInstalls(const QString &message);

    const quint16 m_localPort;
    QWebSocket *m_socket;
    QTimer *m_retryTimer;
    bool m_automaticReconnect = true;
    bool m_authenticated = false;
    bool m_modelListAvailable = false;
    QList<ManagedModelInfo> m_generationModels;
    QList<ManagedModelInfo> m_editionModels;
    QList<ManagedModelInfo> m_upscaleModels;
    QList<ManagedModelInfo> m_llmChatModels;
    QList<ManagedModelInfo> m_ipAdapterModels;
    QList<ManagedInstall> m_installs;
};
