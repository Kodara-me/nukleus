#include "LlamaServerRunner.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUuid>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#endif

namespace
{
constexpr int HealthPollMs = 250;
constexpr qint64 MaximumReplyBytes = 1024 * 1024;
constexpr int LogTailBytes = 4096;

LlamaServerReply loadedReply()
{
    LlamaServerReply reply;
    reply.succeeded = true;
    return reply;
}

quint16 freeLoopbackPort()
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, 0) ? probe.serverPort() : 0;
}
}

LlamaServerRunner::LlamaServerRunner(QString executable, int idleMs, QObject *parent)
    : QObject(parent)
    , m_executable(std::move(executable))
{
    qRegisterMetaType<LlamaServerReply>();
    m_process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_LINUX
    // A crashed daemon must not leave the model holding GPU memory.
    m_process.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_log = (m_log + m_process.readAllStandardOutput()).right(LogTailBytes);
    });
    connect(&m_process, &QProcess::finished, this, &LlamaServerRunner::handleProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_active)
            fail(QStringLiteral("chat-failed"), tr("Could not start llama-server: %1").arg(m_process.errorString()));
    });
    m_healthTimer.setInterval(HealthPollMs);
    connect(&m_healthTimer, &QTimer::timeout, this, &LlamaServerRunner::pollHealth);
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(idleMs);
    connect(&m_idleTimer, &QTimer::timeout, this, &LlamaServerRunner::stop);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_active) return;
        abortRequest();
        m_process.kill();
        fail(QStringLiteral("chat-timeout"), tr("The text chat did not finish in time"));
    });
}

LlamaServerRunner::~LlamaServerRunner()
{
    m_active = false;
    stop();
}

bool LlamaServerRunner::isAvailable() const
{
    return !m_executable.isEmpty() && !QStandardPaths::findExecutable(m_executable).isEmpty();
}

bool LlamaServerRunner::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

void LlamaServerRunner::run(const LlamaServerLaunch &launch, const QJsonObject &body, int timeoutMs)
{
    m_idleTimer.stop();
    const bool reuse = isRunning() && m_ready && m_launch == launch;
    // Unload a different model before the request becomes active, so its exit is not reported as a failure.
    if (!reuse) stop();
    m_active = true;
    m_body = body;
    m_outputExceeded = false;
    m_timeout.start(timeoutMs);
    if (reuse) {
        m_coldStart = false;
        if (m_body.isEmpty()) finish(loadedReply());
        else sendRequest();
        return;
    }
    m_launch = launch;
    m_coldStart = true;
    m_startAttempts = 0;
    startProcess();
}

void LlamaServerRunner::cancel()
{
    if (!m_active) return;
    abortRequest();
    m_process.kill();
    fail(QStringLiteral("chat-canceled"), tr("The text chat was canceled"));
}

void LlamaServerRunner::stop()
{
    m_idleTimer.stop();
    m_healthTimer.stop();
    abortRequest();
    if (isRunning()) {
        m_process.terminate();
        if (!m_process.waitForFinished(5000)) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
    m_ready = false;
}

void LlamaServerRunner::startProcess()
{
    ++m_startAttempts;
    m_port = freeLoopbackPort();
    // The key stays out of the command line, which other local users can read.
    m_keyDirectory = std::make_unique<QTemporaryDir>();
    m_apiKey = QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
    QFile keyFile(m_keyDirectory->filePath(QStringLiteral("api-key")));
    if (m_port == 0 || !m_keyDirectory->isValid() || !keyFile.open(QIODevice::WriteOnly)
        || keyFile.write(m_apiKey.toUtf8()) != m_apiKey.size()) {
        fail(QStringLiteral("chat-failed"), tr("Could not prepare llama-server"));
        return;
    }
    keyFile.close();
    QStringList arguments{"--model", m_launch.modelPath, "--ctx-size", QString::number(m_launch.contextLength)};
    arguments += m_launch.arguments;
    arguments += {"--host", "127.0.0.1", "--port", QString::number(m_port), "--api-key-file", keyFile.fileName(),
        "--no-webui", "--parallel", "1", "--jinja"};
    m_log.clear();
    m_ready = false;
    m_process.start(m_executable, arguments);
    m_healthTimer.start();
}

QNetworkRequest LlamaServerRunner::request(const QString &path) const
{
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return request;
}

void LlamaServerRunner::pollHealth()
{
    if (m_healthReply || !isRunning()) return;
    m_healthReply = m_network.get(request(QStringLiteral("/health")));
    connect(m_healthReply, &QNetworkReply::finished, this, [this, reply = m_healthReply] {
        reply->deleteLater();
        // 503 while the model loads; connection errors until the listener is up.
        if (!m_active || m_ready || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        m_ready = true;
        m_healthTimer.stop();
        if (m_body.isEmpty()) finish(loadedReply());
        else sendRequest();
    });
}

void LlamaServerRunner::sendRequest()
{
    m_reply = m_network.post(request(QStringLiteral("/v1/chat/completions")),
                             QJsonDocument(m_body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, [this, reply = m_reply] {
        if (reply->bytesAvailable() <= MaximumReplyBytes) return;
        m_outputExceeded = true;
        reply->abort();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply] { handleReply(reply); });
}

void LlamaServerRunner::handleReply(QNetworkReply *reply)
{
    reply->deleteLater();
    if (!m_active || reply != m_reply) return;
    m_reply.clear();
    if (m_outputExceeded) {
        fail(QStringLiteral("chat-output-too-large"), tr("The text chat reply exceeded 1 MiB"));
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray bytes = reply->readAll();
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();
    const QJsonObject error = object.value("error").toObject();
    if (status == 200 && !object.value("choices").toArray().isEmpty()) {
        const QJsonObject choice = object.value("choices").toArray().first().toObject();
        const QJsonObject usage = object.value("usage").toObject();
        LlamaServerReply result;
        result.succeeded = true;
        result.content = choice.value("message").toObject().value("content").toString();
        result.finishReason = choice.value("finish_reason").toString();
        result.promptTokens = usage.value("prompt_tokens").toInt(-1);
        result.completionTokens = usage.value("completion_tokens").toInt(-1);
        finish(result);
    } else if (status == 400 && error.value("type").toString() == QStringLiteral("exceed_context_size_error")) {
        fail(QStringLiteral("chat-context-exceeded"), tr("The conversation needs %1 tokens but the context holds %2")
             .arg(error.value("n_prompt_tokens").toInt()).arg(error.value("n_ctx").toInt()));
    } else if (status != 0) {
        const QString detail = error.value("message").toString(QString::fromUtf8(bytes.left(LogTailBytes)));
        fail(QStringLiteral("chat-failed"), tr("llama-server returned HTTP %1: %2").arg(status).arg(detail));
    } else {
        fail(QStringLiteral("chat-failed"), tr("llama-server request failed: %1").arg(reply->errorString()) + diagnostics());
    }
}

void LlamaServerRunner::handleProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    m_log = (m_log + m_process.readAllStandardOutput()).right(LogTailBytes);
    const bool wasReady = m_ready;
    m_ready = false;
    m_healthTimer.stop();
    if (!m_active) return;
    // Another process may take the probed port before llama-server binds it; try a fresh one once.
    if (!wasReady && m_startAttempts < 2 && m_log.contains("bind")) {
        startProcess();
        return;
    }
    abortRequest();
    const QString reason = status == QProcess::NormalExit ? tr("exit code %1").arg(exitCode) : tr("crashed");
    fail(QStringLiteral("chat-failed"), tr("llama-server stopped (%1)").arg(reason) + diagnostics());
}

void LlamaServerRunner::abortRequest()
{
    for (const auto &reply : {m_reply, m_healthReply}) {
        if (!reply) continue;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_reply.clear();
    m_healthReply.clear();
}

QString LlamaServerRunner::diagnostics() const
{
    const QString log = QString::fromUtf8(m_log).trimmed();
    return log.isEmpty() ? QString() : QLatin1Char('\n') + log;
}

void LlamaServerRunner::fail(const QString &code, const QString &message)
{
    LlamaServerReply reply;
    reply.errorCode = code;
    reply.message = message;
    finish(reply);
}

void LlamaServerRunner::finish(LlamaServerReply reply)
{
    if (!m_active) return;
    m_active = false;
    m_timeout.stop();
    m_healthTimer.stop();
    reply.coldStart = m_coldStart;
    if (isRunning() && m_ready) m_idleTimer.start();
    emit finished(reply);
}
