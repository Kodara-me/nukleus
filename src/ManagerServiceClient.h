#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

#include <optional>

class QWebSocket;

struct ManagedModelInfo
{
    QString id;
    QString name;
    QString fileName;
    qint64 fileSize = 0;
};

enum class ManagedInstallState { Requesting, Accepted, Queued, Downloading, Verifying, Completed, Failed };

struct ManagedInstall
{
    QString installId;
    QString category;
    QUrl url;
    ManagedInstallState state = ManagedInstallState::Requesting;
    qint64 bytesDownloaded = 0;
    std::optional<qint64> totalBytes;
    QString message;
};

class ManagerServiceClient final : public QObject
{
    Q_OBJECT

public:
    explicit ManagerServiceClient(QObject *parent = nullptr);

    QUrl endpoint() const;
    QString token() const;
    void setConfiguration(const QUrl &endpoint, const QString &token, bool persist = true);
    void connectToService();
    bool isAuthenticated() const;
    bool modelListAvailable() const;
    QList<ManagedModelInfo> models(const QString &category) const;
    QList<ManagedInstall> installs() const;
    void requestModels();
    bool installModel(const QString &category, const QUrl &url, const QString &sha256 = {});

signals:
    void serviceStateChanged(const QString &state, const QString &message);
    void modelsChanged();
    void installsChanged();

private:
    void authenticate();
    void handleTextMessage(const QString &message);
    ManagedInstall *findInstall(const QString &installId);
    void failActiveInstalls(const QString &message);

    QWebSocket *m_socket;
    QUrl m_endpoint;
    QString m_token;
    bool m_authenticated = false;
    bool m_modelListAvailable = false;
    QList<ManagedModelInfo> m_generationModels;
    QList<ManagedModelInfo> m_upscaleModels;
    QList<ManagedModelInfo> m_llmChatModels;
    QList<ManagedInstall> m_installs;
};
