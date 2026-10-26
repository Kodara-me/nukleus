#pragma once
#include "BackendLink.h"
#include "HttpServer.h"
#include "WebConfig.h"

#include <QObject>

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
    HttpResponse statusResponse() const;

    WebConfig m_config;
    BackendLink m_link;
    HttpServer m_server;
};
}
