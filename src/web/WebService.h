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
    QJsonObject textToImageAction() const;
    HttpResponse submitJob(const HttpRequest &request);
    HttpResponse jobResponse(const QString &id);
    HttpResponse resultResponse(const QString &id);
    // Asks Nukleus for its models, one request in flight at a time (R3), then calls `then`.
    void withFreshModels(std::function<void()> then);
    void finishModelRefresh();
    void onJobStateChanged(const QString &sdkId, Nukleus::JobState state, int queuePosition);
    void onProgress(const QString &sdkId, const Nukleus::GenerationProgress &progress);
    void onImageJobFinished(const Nukleus::ImageJobResult &result);

    WebConfig m_config;
    BackendLink m_link;
    HttpServer m_server;
    JobStore m_jobs;
    QList<std::function<void()>> m_modelWaiters;
    QTimer m_modelTimeout;
};
}
