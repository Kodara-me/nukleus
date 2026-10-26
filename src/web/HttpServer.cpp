#include "HttpServer.h"
#include "HostRules.h"

#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#endif

namespace NukleusWeb
{
namespace
{
constexpr int DrainGraceMs = 1000;

class Listener final : public QTcpServer
{
public:
    explicit Listener(std::function<void(qintptr)> onConnection, QObject *parent)
        : QTcpServer(parent)
        , m_onConnection(std::move(onConnection))
    {
    }

protected:
    void incomingConnection(qintptr descriptor) override { m_onConnection(descriptor); }

private:
    std::function<void(qintptr)> m_onConnection;
};

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}
}

class HttpConnection final : public QObject
{
    Q_OBJECT
public:
    HttpConnection(HttpServer *server, QTcpSocket *socket, bool counted)
        : QObject(server)
        , m_server(server)
        , m_socket(socket)
        , m_counted(counted)
    {
        m_socket->setParent(this);
        m_idle.setSingleShot(true);
        m_drain.setSingleShot(true);
        connect(&m_idle, &QTimer::timeout, this, &HttpConnection::abortConnection);
        connect(&m_drain, &QTimer::timeout, this, &HttpConnection::abortConnection);
        connect(m_socket, &QTcpSocket::readyRead, this, &HttpConnection::onReadyRead);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &HttpConnection::onBytesWritten);
        connect(m_socket, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        if (m_counted) ++m_server->m_connections;
    }

    ~HttpConnection() override
    {
        if (m_counted) --m_server->m_connections;
        if (m_inFlight) --m_server->m_bodiesInFlight;
    }

    // Called once the connection is accepted: the checks that come before reading anything.
    void begin()
    {
        if (!m_counted) {
            respondEarly(503, "The service is busy.");
            return;
        }
        const QHostAddress peer = m_server->m_options.peerAddressOverride.isNull()
            ? m_socket->peerAddress() : m_server->m_options.peerAddressOverride;
        m_peer = peer;
        if (!isAllowedSource(peer)) {
            respondEarly(403, "Forbidden.");
            return;
        }
        m_idle.start(m_server->m_options.idleTimeoutMs);
        if (m_socket->bytesAvailable() > 0) onReadyRead();
    }

private:
    enum class Phase { Head, Body, Handling, Writing, Draining };

    void abortConnection()
    {
        m_socket->abort();
        deleteLater();
    }

    void restartIdle() { m_idle.start(m_server->m_options.idleTimeoutMs); }

    void onReadyRead()
    {
        if (m_phase == Phase::Draining) {
            m_socket->readAll();
            return;
        }
        if (m_phase != Phase::Head && m_phase != Phase::Body) {
            // Nothing more is expected from the client: discard what it sends so the read buffer stays empty.
            m_socket->readAll();
            return;
        }
        const QByteArray data = m_socket->readAll();
        if (data.isEmpty()) return;
        restartIdle();
        if (m_phase == Phase::Head) {
            m_buffer += data;
            parseHead();
        } else {
            m_request.body += data;
            checkBody();
        }
    }

    void parseHead()
    {
        const int limit = m_server->m_options.maxHeaderBytes;
        const int end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buffer.size() > limit) respondEarly(431, "Request header too large.");
            return;
        }
        if (end + 4 > limit) {
            respondEarly(431, "Request header too large.");
            return;
        }
        QList<QByteArray> lines;
        for (qsizetype start = 0; start <= end;) {
            qsizetype stop = m_buffer.indexOf("\r\n", start);
            if (stop < 0 || stop > end) stop = end;
            lines.append(m_buffer.mid(start, stop - start));
            start = stop + 2;
        }
        const QByteArray rest = m_buffer.mid(end + 4);
        m_buffer.clear();

        const QList<QByteArray> parts = lines.first().split(' ');
        if (parts.size() != 3 || !parts.at(2).startsWith("HTTP/1.")) {
            respondEarly(400, "Malformed request.");
            return;
        }
        m_request.method = parts.at(0);
        const QByteArray target = parts.at(1);
        const int cut = target.indexOf('?');
        m_request.path = cut < 0 ? target : target.left(cut);
        if (cut >= 0) m_request.query = target.mid(cut + 1);
        if (!m_request.path.startsWith('/')) {
            respondEarly(400, "Malformed request.");
            return;
        }
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i);
            if (line.contains('\n') || line.contains('\r')) {
                respondEarly(400, "Malformed request.");
                return;
            }
            const int colon = line.indexOf(':');
            if (colon <= 0 || line.at(0) == ' ' || line.at(0) == '\t') {
                respondEarly(400, "Malformed request.");
                return;
            }
            const QByteArray name = line.left(colon).trimmed().toLower();
            if (m_request.headers.contains(name) && (name == "host" || name == "content-length")) {
                respondEarly(400, "Malformed request.");
                return;
            }
            m_request.headers.insert(name, line.mid(colon + 1).trimmed());
        }
        m_request.peer = m_peer;

        const bool isPost = m_request.method == "POST";
        const QStringList hosts = m_server->m_acceptedHosts;
        const QString host = QString::fromLatin1(m_request.headers.value("host")).toLower();
        if (!m_request.headers.contains("host") || !hosts.contains(host)) {
            respondEarly(403, "Forbidden.");
            return;
        }
        if (isPost && m_request.headers.contains("origin")) {
            const QString origin = QString::fromLatin1(m_request.headers.value("origin")).toLower();
            bool accepted = false;
            for (const QString &value : hosts)
                if (origin == QStringLiteral("http://") + value) accepted = true;
            if (!accepted) {
                respondEarly(403, "Forbidden.");
                return;
            }
        }
        if (m_request.headers.contains("transfer-encoding")) {
            respondEarly(501, "Transfer-Encoding is not supported.");
            return;
        }
        qint64 length = 0;
        if (m_request.headers.contains("content-length")) {
            bool ok = false;
            const QByteArray value = m_request.headers.value("content-length");
            length = value.toLongLong(&ok);
            if (!ok || length < 0 || value.startsWith('+') || value.startsWith('-')) {
                respondEarly(400, "Malformed request.");
                return;
            }
        } else if (isPost) {
            respondEarly(411, "Content-Length is required.");
            return;
        }
        if (length > m_server->m_options.maxBodyBytes) {
            respondEarly(413, "Request body too large.");
            return;
        }
        if (m_request.method != "GET" && !isPost) {
            m_allow = true;
            respondEarly(405, "Only GET and POST are supported.");
            return;
        }
        if (!isPost && length > 0) {
            respondEarly(400, "A GET request has no body.");
            return;
        }
        if (rest.size() > length) {
            respondEarly(400, "Malformed request.");
            return;
        }
        m_bodyLength = length;
        m_request.body = rest;
        m_phase = Phase::Body;
        if (m_request.body.size() < m_bodyLength) {
            if (m_server->m_bodiesInFlight >= m_server->m_options.maxBodiesInFlight) {
                respondEarly(503, "The service is busy.");
                return;
            }
            ++m_server->m_bodiesInFlight;
            m_inFlight = true;
        }
        checkBody();
    }

    void checkBody()
    {
        if (m_request.body.size() > m_bodyLength) {
            respondEarly(400, "Malformed request.");
            return;
        }
        if (m_request.body.size() < m_bodyLength) return;
        if (m_inFlight) {
            m_inFlight = false;
            --m_server->m_bodiesInFlight;
        }
        m_fullyRead = true;
        m_phase = Phase::Handling;
        m_idle.stop();
        QPointer<HttpConnection> self(this);
        m_server->m_handler(m_request, [self](HttpResponse response) {
            if (self) self->send(std::move(response));
        });
    }

    void respondEarly(int status, const QByteArray &message)
    {
        if (m_inFlight) {
            m_inFlight = false;
            --m_server->m_bodiesInFlight;
        }
        m_fullyRead = false;
        HttpResponse response;
        response.status = status;
        response.body = message + "\n";
        if (m_allow) response.headers.emplaceBack("Allow", "GET, POST");
        send(std::move(response));
    }

    void send(HttpResponse response)
    {
        if (m_phase == Phase::Writing || m_phase == Phase::Draining) return;
        m_phase = Phase::Writing;
        QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reasonPhrase(response.status) + "\r\n";
        head += "Content-Type: " + response.contentType + "\r\n";
        head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        head += "Connection: close\r\n";
        for (const auto &header : std::as_const(response.headers)) head += header.first + ": " + header.second + "\r\n";
        head += "\r\n";
        restartIdle();
        m_socket->write(head);
        m_socket->write(response.body);
        onBytesWritten();
    }

    void onBytesWritten()
    {
        if (m_phase != Phase::Writing) return;
        restartIdle();
        if (m_socket->bytesToWrite() > 0) return;
        m_idle.stop();
        if (m_fullyRead) {
            m_socket->disconnectFromHost();
            return;
        }
        // The request was refused before it was read: closing now with unread data would reset the
        // connection and could drop the response, so end our side and let the client finish.
        m_phase = Phase::Draining;
#ifdef Q_OS_UNIX
        ::shutdown(static_cast<int>(m_socket->socketDescriptor()), SHUT_WR);
#endif
        m_socket->readAll();
        m_drain.start(DrainGraceMs);
    }

    HttpServer *m_server;
    QTcpSocket *m_socket;
    bool m_counted;
    bool m_inFlight = false;
    bool m_fullyRead = false;
    bool m_allow = false;
    Phase m_phase = Phase::Head;
    QHostAddress m_peer;
    QByteArray m_buffer;
    HttpRequest m_request;
    qint64 m_bodyLength = 0;
    QTimer m_idle;
    QTimer m_drain;
};

HttpServer::HttpServer(HttpServerOptions options, HttpHandler handler, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_handler(std::move(handler))
{
}

HttpServer::~HttpServer()
{
    // The connections report to the server when they go: delete them while it is still whole.
    qDeleteAll(findChildren<HttpConnection *>(Qt::FindDirectChildrenOnly));
}

bool HttpServer::listen(QString *error)
{
    QList<QHostAddress> addresses{QHostAddress(QHostAddress::LocalHost)};
    if (!m_options.lanAddress.isNull() && m_options.lanAddress != QHostAddress(QHostAddress::LocalHost))
        addresses.append(m_options.lanAddress);
    quint16 port = m_options.port;
    for (const QHostAddress &address : std::as_const(addresses)) {
        auto *listener = new Listener([this](qintptr descriptor) { accept(descriptor); }, this);
        if (!listener->listen(address, port)) {
            if (error) *error = QStringLiteral("Cannot listen on %1:%2: %3").arg(address.toString()).arg(port).arg(listener->errorString());
            delete listener;
            qDeleteAll(m_listeners);
            m_listeners.clear();
            return false;
        }
        port = listener->serverPort();
        m_listeners.append(listener);
    }
    m_port = port;
    m_acceptedHosts = acceptedHostValues(m_options.hostName, m_options.lanAddress, m_port);
    return true;
}

QList<QHostAddress> HttpServer::listenAddresses() const
{
    QList<QHostAddress> addresses;
    for (const QTcpServer *listener : m_listeners) addresses.append(listener->serverAddress());
    return addresses;
}

QStringList HttpServer::acceptedHosts() const { return m_acceptedHosts; }

void HttpServer::accept(qintptr descriptor)
{
    auto *socket = new QTcpSocket;
    if (!socket->setSocketDescriptor(descriptor)) {
        delete socket;
        return;
    }
    const bool counted = m_connections < m_options.maxConnections;
    auto *connection = new HttpConnection(this, socket, counted);
    connection->begin();
}
}

#include "HttpServer.moc"
