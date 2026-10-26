#pragma once
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QStringList>
#include <functional>
#include <memory>

class QTcpServer;

namespace NukleusWeb
{
struct HttpRequest
{
    QByteArray method;
    QByteArray path;                      // the target without its query and fragment
    QByteArray query;
    QHash<QByteArray, QByteArray> headers; // names in lower case
    QByteArray body;
    QHostAddress peer;
};

struct HttpResponse
{
    int status = 200;
    QByteArray contentType = "text/plain; charset=utf-8";
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
};

using HttpResponder = std::function<void(HttpResponse)>;
using HttpHandler = std::function<void(const HttpRequest &, HttpResponder)>;

struct HttpServerOptions
{
    QHostAddress lanAddress;          // second listen address besides 127.0.0.1, null for none
    quint16 port = 9385;              // 0 picks a free port (tests)
    QString hostName;                 // the name the `Host` header may carry (R26)
    int idleTimeoutMs = 60000;        // R29
    int maxConnections = 16;          // R33
    int maxHeaderBytes = 16 * 1024;   // R30
    qint64 maxBodyBytes = 100LL * 1024 * 1024; // R18
    int maxBodiesInFlight = 2;        // R34
    QHostAddress peerAddressOverride; // tests only: classify every peer as this address
};

// A minimal HTTP/1.1 server: one request per connection, GET and POST only.
class HttpServer final : public QObject
{
    Q_OBJECT
public:
    HttpServer(HttpServerOptions options, HttpHandler handler, QObject *parent = nullptr);
    ~HttpServer() override;

    bool listen(QString *error);
    quint16 port() const { return m_port; }
    QList<QHostAddress> listenAddresses() const;
    QStringList acceptedHosts() const;
    int openConnections() const { return m_connections; }
    int bodiesInFlight() const { return m_bodiesInFlight; }

private:
    friend class HttpConnection;
    void accept(qintptr descriptor);

    HttpServerOptions m_options;
    HttpHandler m_handler;
    QList<QTcpServer *> m_listeners;
    quint16 m_port = 0;
    QStringList m_acceptedHosts;
    int m_connections = 0;
    int m_bodiesInFlight = 0;
};
}
