#include "WebService.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace NukleusWeb
{
namespace
{
struct StaticFile
{
    const char *path;
    const char *resource;
    const char *mimeType;
};

constexpr StaticFile StaticFiles[] = {
    {"/", ":/ui/index.html", "text/html; charset=utf-8"},
    {"/index.html", ":/ui/index.html", "text/html; charset=utf-8"},
    {"/app.css", ":/ui/app.css", "text/css; charset=utf-8"},
    {"/app.js", ":/ui/app.js", "text/javascript; charset=utf-8"},
};

HttpServerOptions serverOptions(const WebConfig &config)
{
    HttpServerOptions options;
    options.lanAddress = config.listenAddress;
    options.port = config.port;
    options.hostName = config.hostName;
    options.idleTimeoutMs = config.idleTimeoutMs;
    return options;
}

void addSecurityHeaders(HttpResponse &response)
{
    response.headers.emplaceBack("X-Content-Type-Options", "nosniff");
    response.headers.emplaceBack("Referrer-Policy", "no-referrer");
    response.headers.emplaceBack("X-Frame-Options", "DENY");
    response.headers.emplaceBack("Content-Security-Policy",
        "default-src 'self'; img-src 'self' blob:; media-src 'self' blob:; frame-ancestors 'none'");
}
}

WebService::WebService(const WebConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_link(config, this)
    , m_server(serverOptions(config), [this](const HttpRequest &request, HttpResponder respond) { handle(request, std::move(respond)); }, this)
{
}

bool WebService::start(QString *error)
{
    if (!m_server.listen(error)) return false;
    QStringList addresses;
    for (const QHostAddress &address : m_server.listenAddresses()) addresses << address.toString();
    qInfo().noquote() << QStringLiteral("[Nukleus Web] Listening on %1 port %2.").arg(addresses.join(QStringLiteral(", "))).arg(m_server.port());
    m_link.start();
    return true;
}

HttpResponse WebService::statusResponse() const
{
    QString state = QStringLiteral("connecting");
    if (m_link.state() == BackendLink::State::Available) state = QStringLiteral("available");
    else if (m_link.state() == BackendLink::State::Unavailable) state = QStringLiteral("unavailable");
    QJsonObject object{{QStringLiteral("state"), state}};
    object.insert(QStringLiteral("reason"), m_link.reason().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m_link.reason()));
    HttpResponse response;
    response.contentType = "application/json";
    response.body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    response.headers.emplaceBack("Cache-Control", "no-store");
    return response;
}

void WebService::handle(const HttpRequest &request, HttpResponder respond)
{
    HttpResponse response;
    if (request.method == "GET" && request.path == "/api/status") {
        response = statusResponse();
    } else if (request.method == "GET") {
        bool found = false;
        for (const StaticFile &file : StaticFiles) {
            if (request.path != file.path) continue;
            QFile resource(QString::fromLatin1(file.resource));
            if (!resource.open(QIODevice::ReadOnly)) break;
            response.contentType = file.mimeType;
            response.body = resource.readAll();
            response.headers.emplaceBack("Cache-Control", "no-cache");
            found = true;
            break;
        }
        if (!found) {
            response.status = 404;
            response.body = "Not found.\n";
        }
    } else {
        response.status = 404;
        response.body = "Not found.\n";
    }
    addSecurityHeaders(response);
    respond(std::move(response));
}
}
