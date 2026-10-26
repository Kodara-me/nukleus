#pragma once
#include "BackendLink.h"
#include "HttpServer.h"
#include "JobStore.h"
#include "WebConfig.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <functional>

namespace NukleusWeb
{
class WebService final : public QObject
{
    Q_OBJECT
public:
    explicit WebService(const WebConfig &config, QObject *parent = nullptr);

    // Starts the HTTP server and the connection to Nukleus.
    bool start(QString *error);
    HttpServer *server() { return &m_server; }
    BackendLink *link() { return &m_link; }

private:
    void handle(const HttpRequest &request, HttpResponder respond);
    void respondStatus(HttpResponder respond);
    QJsonObject actionJson(int index) const;
    HttpResponse submitJob(const HttpRequest &request);
    HttpResponse jobResponse(const QString &id);
    HttpResponse resultResponse(const QString &id);
    // Asks Nukleus for its models, one request in flight at a time (R3), then calls `then`.
    void withFreshModels(std::function<void()> then);
    void finishModelRefresh();
    void onJobStateChanged(const QString &sdkId, Nukleus::JobState state, int queuePosition);
    void onProgress(const QString &sdkId, const Nukleus::GenerationProgress &progress);
    void onImageJobFinished(const Nukleus::ImageJobResult &result);
    void onAnimationFinished(const Nukleus::AnimationResult &result);
    void finishJob(const QString &sdkId, bool succeeded, const QByteArray &data, const QByteArray &mimeType, const QString &message);
    // The submission queue (R35): jobs wait here, with their images, until the previous job's uploads are done.
    using Sender = std::function<QString()>;
    void enqueue(const QString &jobId, Sender send);
    void sendNext();
    void releaseSending(const QString &sdkId);

    WebConfig m_config;
    BackendLink m_link;
    HttpServer m_server;
    JobStore m_jobs;
    struct Waiting
    {
        QString jobId;
        std::function<QString()> send;
    };
    QList<Waiting> m_waiting;
    QString m_sendingSdkId; // the job whose images are being sent, empty when none
    bool m_sending = false;
    QList<std::function<void()>> m_modelWaiters;
    QTimer m_modelTimeout;
};
}
