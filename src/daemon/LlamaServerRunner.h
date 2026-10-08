#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

class QNetworkReply;

// Launch settings of a llama-server process; a request with different settings restarts it.
struct LlamaServerLaunch
{
    QString modelPath;
    int contextLength = 0;
    QStringList arguments; // Model layers, KV cache, attention, chat template and reasoning arguments.
    bool operator==(const LlamaServerLaunch &) const = default;
};

struct LlamaServerReply
{
    bool succeeded = false;
    QString content; // Raw assistant content; the daemon cleans it.
    QString finishReason;
    int promptTokens = -1;
    int completionTokens = -1;
    bool coldStart = false; // The request loaded the model.
    QString errorCode;
    QString message;
};

// Owns one loopback llama-server child, keeps its model loaded between chat requests and
// unloads it on demand or after an idle period.
class LlamaServerRunner final : public QObject
{
    Q_OBJECT

public:
    LlamaServerRunner(QString executable, int idleMs, QObject *parent = nullptr);
    ~LlamaServerRunner() override;
    bool isAvailable() const;
    bool isRunning() const;
    // Starts or reuses the server, waits until it is healthy and posts one chat completion.
    void run(const LlamaServerLaunch &launch, const QJsonObject &body, int timeoutMs);
    // Abandons the active request and kills the server; finished() reports chat-canceled.
    void cancel();
    // Unloads the model synchronously; used before other GPU work and after the idle period.
    void stop();

signals:
    void finished(const LlamaServerReply &reply);

private:
    void startProcess();
    void pollHealth();
    void sendRequest();
    void handleReply(QNetworkReply *reply);
    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);
    void fail(const QString &code, const QString &message);
    void finish(LlamaServerReply reply);
    void abortRequest();
    QString diagnostics() const;
    QNetworkRequest request(const QString &path) const;

    QString m_executable;
    QProcess m_process;
    QNetworkAccessManager m_network;
    QTimer m_healthTimer;
    QTimer m_idleTimer;
    QTimer m_timeout;
    QPointer<QNetworkReply> m_reply;
    QPointer<QNetworkReply> m_healthReply;
    std::unique_ptr<QTemporaryDir> m_keyDirectory;
    LlamaServerLaunch m_launch;
    QJsonObject m_body;
    QString m_apiKey;
    quint16 m_port = 0;
    QByteArray m_log;
    bool m_active = false;
    bool m_ready = false;
    bool m_coldStart = false;
    bool m_outputExceeded = false;
    int m_startAttempts = 0;
};

Q_DECLARE_METATYPE(LlamaServerReply)
