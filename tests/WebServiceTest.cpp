#include "BackendLink.h"
#include "HostRules.h"
#include "HttpServer.h"
#include "WebConfig.h"
#include "WebService.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

#include <memory>

using namespace NukleusWeb;

namespace
{
QStringList g_log;
QtMessageHandler g_previousHandler = nullptr;

void captureLog(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    g_log.append(message);
    if (g_previousHandler) g_previousHandler(type, context, message);
}

// A stand-in for nukleusd: a WebSocket server speaking protocol v3's authentication.
class FakeDaemon final : public QObject
{
public:
    enum class Mode { Accept, RejectAuthentication, Incompatible };

    FakeDaemon() = default;

    bool start(quint16 port = 0)
    {
        m_server = std::make_unique<QWebSocketServer>(QStringLiteral("fake-nukleusd"), QWebSocketServer::NonSecureMode);
        connect(m_server.get(), &QWebSocketServer::newConnection, this, [this] {
            QWebSocket *socket = m_server->nextPendingConnection();
            m_sockets.append(socket);
            connect(socket, &QWebSocket::disconnected, this, [this, socket] {
                m_sockets.removeAll(socket);
                socket->deleteLater();
            });
            connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &text) {
                const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
                if (message.value("type") == "models.list") {
                    ++m_modelRequests;
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.list.result"}, {"categories", m_categories}}).toJson(QJsonDocument::Compact)));
                    return;
                }
                if (message.value("type") == "submit") {
                    m_submits.append(message);
                    m_submitSocket = socket;
                    const QString jobId = message.value("jobId").toString();
                    if (m_rejectSubmissions)
                        send(socket, {{"type", "error"}, {"code", "invalid-request"}, {"message", "Invalid generation options"}, {"jobId", jobId}});
                    else
                        send(socket, {{"type", "queued"}, {"jobId", jobId}});
                    return;
                }
                if (message.value("type") != "authenticate") return;
                m_tokens.append(message.value("token").toString());
                if (m_mode == Mode::RejectAuthentication) {
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"code\":\"authentication-failed\",\"message\":\"Authentication failed\"}"));
                    socket->close();
                } else if (m_mode == Mode::Incompatible) {
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"code\":\"incompatible-protocol\",\"message\":\"Unsupported protocol version\"}"));
                } else {
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"authenticated\"}"));
                    send(socket, {{"type", "capabilities"}, {"state", "ready"}, {"operations", QJsonArray::fromStringList(m_operations)}});
                }
            });
        });
        return m_server->listen(QHostAddress::LocalHost, port);
    }

    void stop()
    {
        if (!m_server) return;
        m_server->close();
        const auto sockets = m_sockets;
        for (QWebSocket *socket : sockets) socket->close();
    }

    quint16 port() const { return m_server->serverPort(); }
    void setMode(Mode mode) { m_mode = mode; }
    QStringList tokens() const { return m_tokens; }

    void setOperations(const QStringList &operations) { m_operations = operations; }
    void setModels(const QJsonArray &categories) { m_categories = categories; }
    void setRejectSubmissions(bool reject) { m_rejectSubmissions = reject; }
    int modelRequests() const { return m_modelRequests; }
    QList<QJsonObject> submits() const { return m_submits; }
    QString lastJobId() const { return m_submits.isEmpty() ? QString() : m_submits.last().value("jobId").toString(); }

    // Job events of the last submission, as nukleusd sends them.
    void queuePosition(int position) { send(m_submitSocket, {{"type", "queue-position"}, {"jobId", lastJobId()}, {"position", position}}); }
    void running() { send(m_submitSocket, {{"type", "running"}, {"jobId", lastJobId()}}); }
    void progress(int percentage)
    {
        send(m_submitSocket, {{"type", "progress"}, {"jobId", lastJobId()},
                              {"progress", QJsonObject{{"stage", "sampling"}, {"percentage", percentage}, {"statusText", "Sampling"}}}});
    }
    void finish(const QByteArray &bytes, const QString &mimeType = QStringLiteral("image/png"))
    {
        send(m_submitSocket, {{"type", "result"}, {"jobId", lastJobId()}, {"operation", "image.generate"}, {"mimeType", mimeType}});
        m_submitSocket->sendBinaryMessage(bytes);
    }
    void fail(const QString &code, const QString &message)
    {
        send(m_submitSocket, {{"type", "error"}, {"code", code}, {"message", message}, {"jobId", lastJobId()}});
    }

private:
    static void send(QWebSocket *socket, const QJsonObject &object)
    {
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    }

    QList<QWebSocket *> m_sockets;
    QStringList m_operations{QStringLiteral("image.generate")};
    QJsonArray m_categories;
    QList<QJsonObject> m_submits;
    QWebSocket *m_submitSocket = nullptr;
    bool m_rejectSubmissions = false;
    int m_modelRequests = 0;
    std::unique_ptr<QWebSocketServer> m_server;
    QStringList m_tokens;
    Mode m_mode = Mode::Accept;
};

struct Reply
{
    int status = 0;
    QByteArray head;
    QByteArray body;
    bool complete = false;
    bool closed = false;
    QByteArray header(const QByteArray &name) const
    {
        for (const QByteArray &line : head.split('\n')) {
            if (line.toLower().startsWith(name.toLower() + ':')) return line.mid(name.size() + 1).trimmed();
        }
        return {};
    }
};

Reply parseReply(const QByteArray &data)
{
    Reply reply;
    const int end = data.indexOf("\r\n\r\n");
    if (end < 0) return reply;
    reply.head = data.left(end);
    reply.status = reply.head.split(' ').value(1).toInt();
    reply.body = data.mid(end + 4);
    const QByteArray length = reply.header("Content-Length");
    reply.complete = !length.isEmpty() && reply.body.size() >= length.toLongLong();
    return reply;
}

// Sends `raw` to the server and reads until the response is complete or the server closes.
Reply exchange(const QHostAddress &address, quint16 port, const QByteArray &raw, int timeoutMs = 5000)
{
    QTcpSocket socket;
    QByteArray data;
    QEventLoop loop;
    bool closed = false;
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&] {
        data += socket.readAll();
        if (parseReply(data).complete) loop.quit();
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, [&] {
        data += socket.readAll();
        closed = true;
        loop.quit();
    });
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] { socket.write(raw); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    socket.connectToHost(address, port);
    loop.exec();
    Reply reply = parseReply(data);
    reply.closed = closed;
    return reply;
}

QByteArray request(const QByteArray &method, const QByteArray &path, const QList<QByteArray> &headers, const QByteArray &body = {})
{
    QByteArray raw = method + ' ' + path + " HTTP/1.1\r\n";
    for (const QByteArray &header : headers) raw += header + "\r\n";
    raw += "\r\n" + body;
    return raw;
}

QByteArray hostHeader(quint16 port, const QByteArray &name = "127.0.0.1")
{
    return "Host: " + name + ':' + QByteArray::number(port);
}

HttpHandler okHandler()
{
    return [](const HttpRequest &, HttpResponder respond) {
        HttpResponse response;
        response.body = "ok";
        respond(std::move(response));
    };
}

struct TestServer
{
    explicit TestServer(HttpServerOptions options, HttpHandler handler = okHandler())
        : server(std::move(options), std::move(handler))
    {
        QString error;
        listening = server.listen(&error);
        if (!listening) qWarning() << error;
    }
    HttpServer server;
    bool listening = false;
    quint16 port() const { return server.port(); }
};

HttpServerOptions testOptions()
{
    HttpServerOptions options;
    options.port = 0;
    options.hostName = QStringLiteral("atlas.home.lan");
    options.idleTimeoutMs = 5000;
    return options;
}

WebConfig testConfig(quint16 backendPort)
{
    WebConfig config;
    config.port = 0;
    config.hostName = QStringLiteral("atlas.home.lan");
    config.backendHost = QStringLiteral("127.0.0.1");
    config.backendPort = backendPort;
    config.idleTimeoutMs = 5000;
    config.reconnectIntervalMs = 50;
    return config;
}

quint16 freePort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    return probe.serverPort();
}

QJsonObject imageModel(const QString &id, const QString &displayName, const QJsonObject &recommended = {})
{
    QJsonObject model{{"id", id}, {"name", id + ".safetensors"}, {"fileName", id + ".safetensors"}, {"bytes", 1}, {"displayName", displayName}};
    if (!recommended.isEmpty()) model.insert("recommendedOptions", recommended);
    return model;
}

QJsonArray categories(const QJsonArray &imageModels, const QJsonArray &animationModels = {}, const QJsonArray &upscalers = {})
{
    return {QJsonObject{{"category", "image-generation"}, {"models", imageModels}},
            QJsonObject{{"category", "upscaler"}, {"models", upscalers}},
            QJsonObject{{"category", "animation"}, {"models", animationModels}}};
}

// A `multipart/form-data` body of text fields.
QByteArray multipartBody(const QList<QPair<QString, QString>> &fields)
{
    QByteArray body;
    for (const auto &field : fields)
        body += "--XBOUNDARYX\r\nContent-Disposition: form-data; name=\"" + field.first.toUtf8() + "\"\r\n\r\n" + field.second.toUtf8() + "\r\n";
    return body + "--XBOUNDARYX--\r\n";
}

Reply postJob(quint16 port, QList<QPair<QString, QString>> fields)
{
    const QByteArray body = multipartBody(fields);
    return exchange(QHostAddress::LocalHost, port,
                    request("POST", "/api/jobs", {hostHeader(port), "Content-Type: multipart/form-data; boundary=XBOUNDARYX",
                                                  "Content-Length: " + QByteArray::number(body.size())}, body));
}

QJsonObject jsonOf(const Reply &reply) { return QJsonDocument::fromJson(reply.body).object(); }

Reply getPath(quint16 port, const QByteArray &path) { return exchange(QHostAddress::LocalHost, port, request("GET", path, {hostHeader(port)})); }

QJsonObject jobOf(quint16 port, const QString &id) { return jsonOf(getPath(port, "/api/jobs/" + id.toLatin1())); }

QJsonObject statusOf(quint16 port)
{
    const Reply reply = exchange(QHostAddress::LocalHost, port, request("GET", "/api/status", {hostHeader(port)}));
    return QJsonDocument::fromJson(reply.body).object();
}
}

class WebServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        g_previousHandler = qInstallMessageHandler(captureLog);
    }

    void cleanupTestCase() { qInstallMessageHandler(g_previousHandler); }

    // R15
    void servesPagesAndApiWithoutSignIn()
    {
        FakeDaemon daemon;
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        for (const QByteArray &path : {QByteArray("/"), QByteArray("/index.html"), QByteArray("/app.css"), QByteArray("/app.js"), QByteArray("/api/status")}) {
            const Reply reply = exchange(QHostAddress::LocalHost, port, request("GET", path, {hostHeader(port)}));
            QVERIFY2(reply.status == 200, path.constData());
            QVERIFY(!reply.header("WWW-Authenticate").size());
        }
        QVERIFY(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port)})).body.contains("Nukleus"));
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/missing", {hostHeader(port)})).status, 404);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("PUT", "/", {hostHeader(port), "Content-Length: 0"})).status, 405);
    }

    // R16
    void tokenIsChosenByBackendHost()
    {
        QCOMPARE(tokenForBackendHost("127.0.0.1", "secret"), QString());
        QCOMPARE(tokenForBackendHost("localhost", "secret"), QString());
        QCOMPARE(tokenForBackendHost("::1", "secret"), QString());
        QCOMPARE(tokenForBackendHost("192.168.1.20", "secret"), QString("secret"));
        QCOMPARE(tokenForBackendHost("gpu.home.lan", "secret"), QString("secret"));
        QVERIFY(isLoopbackHost("127.0.0.1"));
        QVERIFY(!isLoopbackHost("10.0.0.2"));
    }

    // R16
    void loopbackBackendIsReachedWithoutToken()
    {
        FakeDaemon daemon;
        QVERIFY(daemon.start());
        WebConfig config = testConfig(daemon.port());
        config.backendToken = QStringLiteral("configured-token");
        WebService service(config);
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        QTRY_COMPARE(statusOf(service.server()->port()).value("state").toString(), QString("available"));
        QCOMPARE(daemon.tokens(), QStringList{QString()});
    }

    // R16
    void configurationReadsTokenFileForRemoteHostOnly()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath("token"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("lan-token\n");
        file.close();

        WebConfig config;
        QString error;
        QProcessEnvironment environment;
        environment.insert("NUKLEUS_WEB_BACKEND_HOST", "192.168.1.20");
        QVERIFY(!WebConfig::fromEnvironment(environment, &config, &error));
        environment.insert("NUKLEUS_WEB_BACKEND_TOKEN_FILE", file.fileName());
        QVERIFY2(WebConfig::fromEnvironment(environment, &config, &error), qPrintable(error));
        QCOMPARE(config.backendToken, QString("lan-token"));
        QCOMPARE(config.backendHost, QString("192.168.1.20"));

        QProcessEnvironment loopback;
        QVERIFY2(WebConfig::fromEnvironment(loopback, &config, &error), qPrintable(error));
        QCOMPARE(config.backendHost, QString("127.0.0.1"));
        QVERIFY(config.backendToken.isEmpty());

        QProcessEnvironment missing;
        missing.insert("NUKLEUS_WEB_BACKEND_HOST", "192.168.1.20");
        missing.insert("NUKLEUS_WEB_BACKEND_TOKEN_FILE", dir.filePath("absent"));
        QVERIFY(!WebConfig::fromEnvironment(missing, &config, &error));
        QVERIFY(!error.contains("lan-token"));

        QProcessEnvironment badListen;
        badListen.insert("NUKLEUS_WEB_LISTEN", "0.0.0.0");
        QVERIFY(!WebConfig::fromEnvironment(badListen, &config, &error));
        QProcessEnvironment lan;
        lan.insert("NUKLEUS_WEB_LISTEN", "192.168.1.20");
        QVERIFY2(WebConfig::fromEnvironment(lan, &config, &error), qPrintable(error));
        QCOMPARE(config.listenAddress, QHostAddress("192.168.1.20"));
        QProcessEnvironment publicListen;
        publicListen.insert("NUKLEUS_WEB_LISTEN", "8.8.8.8");
        QVERIFY(!WebConfig::fromEnvironment(publicListen, &config, &error));
        QCOMPARE(config.port, quint16(9385));
    }

    // R17
    void unavailableStateGivesItsReason_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("reason");
        QTest::newRow("authentication refused") << int(FakeDaemon::Mode::RejectAuthentication) << QString("authentication-refused");
        QTest::newRow("incompatible protocol") << int(FakeDaemon::Mode::Incompatible) << QString("incompatible-protocol");
    }

    void unavailableStateGivesItsReason()
    {
        QFETCH(int, mode);
        QFETCH(QString, reason);
        FakeDaemon daemon;
        daemon.setMode(FakeDaemon::Mode(mode));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("reason").toString(), reason);
        QCOMPARE(statusOf(port).value("state").toString(), QString("unavailable"));
        // The service keeps retrying and recovers once the daemon accepts it.
        daemon.setMode(FakeDaemon::Mode::Accept);
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(port).value("state").toString(), QString("available"), 10000);
        QVERIFY(statusOf(port).value("reason").isNull());
    }

    // R17
    void unreachableThenReconnectsWithoutRestart()
    {
        const quint16 backendPort = freePort();
        WebService service(testConfig(backendPort));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("reason").toString(), QString("unreachable"));
        QCOMPARE(statusOf(port).value("state").toString(), QString("unavailable"));

        FakeDaemon daemon;
        QVERIFY(daemon.start(backendPort));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(port).value("state").toString(), QString("available"), 10000);

        daemon.stop();
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(port).value("state").toString(), QString("unavailable"), 10000);
        QCOMPARE(statusOf(port).value("reason").toString(), QString("unreachable"));
        QVERIFY(daemon.start(backendPort));
        QTRY_COMPARE_WITH_TIMEOUT(statusOf(port).value("state").toString(), QString("available"), 10000);
    }

    // R24
    void listensOnLoopbackAndTheConfiguredAddressOnly()
    {
        QCOMPARE(WebConfig().port, quint16(9385));
        HttpServerOptions options = testOptions();
        options.lanAddress = QHostAddress("127.0.0.2");
        TestServer lan(options);
        QVERIFY(lan.listening);
        QCOMPARE(lan.server.listenAddresses(), (QList<QHostAddress>{QHostAddress("127.0.0.1"), QHostAddress("127.0.0.2")}));
        const quint16 port = lan.port();
        QCOMPARE(exchange(QHostAddress("127.0.0.1"), port, request("GET", "/", {hostHeader(port)})).status, 200);
        QCOMPARE(exchange(QHostAddress("127.0.0.2"), port, request("GET", "/", {hostHeader(port, "127.0.0.2")})).status, 200);
        QCOMPARE(exchange(QHostAddress("127.0.0.3"), port, request("GET", "/", {hostHeader(port)}), 1000).status, 0);

        TestServer loopbackOnly(testOptions());
        QCOMPARE(loopbackOnly.server.listenAddresses(), (QList<QHostAddress>{QHostAddress("127.0.0.1")}));
        QCOMPARE(exchange(QHostAddress("127.0.0.2"), loopbackOnly.port(), request("GET", "/", {hostHeader(loopbackOnly.port())}), 1000).status, 0);
    }

    // R25
    void sourceAddressClassification_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("loopback") << "127.0.0.1" << true;
        QTest::newRow("loopback v6") << "::1" << true;
        QTest::newRow("10/8") << "10.20.30.40" << true;
        QTest::newRow("172.16/12 low") << "172.16.0.1" << true;
        QTest::newRow("172.16/12 high") << "172.31.255.255" << true;
        QTest::newRow("172.32 is public") << "172.32.0.1" << false;
        QTest::newRow("192.168/16") << "192.168.1.5" << true;
        QTest::newRow("169.254/16") << "169.254.10.10" << true;
        QTest::newRow("public") << "8.8.8.8" << false;
        QTest::newRow("shared address space") << "100.64.0.1" << false;
        QTest::newRow("mapped private") << "::ffff:10.0.0.1" << true;
        QTest::newRow("mapped public") << "::ffff:8.8.8.8" << false;
        QTest::newRow("global v6") << "2001:db8::1" << false;
    }

    void sourceAddressClassification()
    {
        QFETCH(QString, address);
        QFETCH(bool, allowed);
        QCOMPARE(isAllowedSource(QHostAddress(address)), allowed);
        // The server applies the same function to the peer, here replaced by the address under test.
        HttpServerOptions options = testOptions();
        options.peerAddressOverride = QHostAddress(address);
        TestServer server(options);
        const Reply reply = exchange(QHostAddress::LocalHost, server.port(), request("GET", "/", {hostHeader(server.port())}));
        QCOMPARE(reply.status, allowed ? 200 : 403);
    }

    // R26
    void hostHeaderIsChecked_data()
    {
        QTest::addColumn<QByteArray>("host");
        QTest::addColumn<int>("status");
        QTest::newRow("loopback") << QByteArray("127.0.0.1") << 200;
        QTest::newRow("localhost") << QByteArray("localhost") << 200;
        QTest::newRow("lan address") << QByteArray("127.0.0.2") << 200;
        QTest::newRow("full name") << QByteArray("atlas.home.lan") << 200;
        QTest::newRow("first label") << QByteArray("atlas") << 200;
        QTest::newRow("first label .local") << QByteArray("atlas.local") << 200;
        QTest::newRow("case-insensitive") << QByteArray("ATLAS.Local") << 200;
        QTest::newRow("foreign") << QByteArray("evil.example") << 403;
        QTest::newRow("other suffix") << QByteArray("atlas.lan") << 403;
        QTest::newRow("other ip") << QByteArray("127.0.0.9") << 403;
    }

    void hostHeaderIsChecked()
    {
        QFETCH(QByteArray, host);
        QFETCH(int, status);
        HttpServerOptions options = testOptions();
        options.lanAddress = QHostAddress("127.0.0.2");
        TestServer server(options);
        const quint16 port = server.port();
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port, host)})).status, status);
    }

    // R26
    void hostHeaderNeedsTheServicePort()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {})).status, 403);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {"Host: atlas"})).status, 403);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {"Host: atlas:" + QByteArray::number(port + 1)})).status, 403);
        QCOMPARE(server.server.acceptedHosts().contains(QString("atlas:%1").arg(port)), true);
        QCOMPARE(server.server.acceptedHosts().contains(QString("atlas.local:%1").arg(port)), true);
    }

    // R27
    void originOfAPostIsChecked_data()
    {
        QTest::addColumn<QByteArray>("origin");
        QTest::addColumn<int>("status");
        QTest::newRow("none") << QByteArray() << 200;
        QTest::newRow("same host") << QByteArray("http://127.0.0.1:%1") << 200;
        QTest::newRow("host name") << QByteArray("http://atlas.local:%1") << 200;
        QTest::newRow("foreign") << QByteArray("http://evil.example") << 403;
        QTest::newRow("foreign with port") << QByteArray("http://evil.example:%1") << 403;
        QTest::newRow("null") << QByteArray("null") << 403;
        QTest::newRow("https") << QByteArray("https://127.0.0.1:%1") << 403;
    }

    void originOfAPostIsChecked()
    {
        QFETCH(QByteArray, origin);
        QFETCH(int, status);
        TestServer server(testOptions());
        const quint16 port = server.port();
        QList<QByteArray> headers{hostHeader(port), "Content-Length: 3"};
        if (!origin.isEmpty()) headers << "Origin: " + origin.replace("%1", QByteArray::number(port));
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("POST", "/x", headers, "abc")).status, status);
        // A GET is not subject to the Origin check.
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port), "Origin: http://evil.example"})).status, 200);
    }

    // R28
    void tokenNeverAppearsInAResponseOrALog()
    {
        const QString token = QStringLiteral("tok-SECRET-0123456789");
        g_log.clear();
        // A backend host that is not loopback, so that the SDK really holds the token.
        WebConfig config = testConfig(freePort());
        config.backendHost = QStringLiteral("nukleus-backend.invalid");
        config.backendToken = token;
        WebService service(config);
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        QCOMPARE(service.link()->client()->token(), token);
        const quint16 port = service.server()->port();
        // Let a failed attempt and its log line happen when the name cannot be resolved quickly.
        QTest::qWait(1500);
        QByteArray everything;
        for (const QByteArray &raw : {request("GET", "/", {hostHeader(port)}), request("GET", "/api/status", {hostHeader(port)}),
                 request("GET", "/app.js", {hostHeader(port)}), request("GET", "/missing", {hostHeader(port)}),
                 request("GET", "/", {"Host: evil.example"}), request("POST", "/x", {hostHeader(port)})}) {
            const Reply reply = exchange(QHostAddress::LocalHost, port, raw);
            everything += reply.head + reply.body;
        }
        QVERIFY(!everything.contains(token.toUtf8()));
        QVERIFY(!g_log.join('\n').contains(token));
        QVERIFY(!g_log.isEmpty());
    }

    // R17
    void reasonIsKeptWhileReconnecting()
    {
        const quint16 backendPort = freePort();
        WebService service(testConfig(backendPort));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("reason").toString(), QString("unreachable"));
        // A host that accepts the connection and never answers the handshake: attempts stay pending.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, backendPort));
        QTRY_VERIFY_WITH_TIMEOUT(silent.hasPendingConnections(), 5000);
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(50);
            const QJsonObject status = statusOf(port);
            QCOMPARE(status.value("state").toString(), QString("unavailable"));
            QCOMPARE(status.value("reason").toString(), QString("unreachable"));
        }
    }

    // R18
    void bodyOverTheLimitIsRefusedBeforeItIsRead()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        // Only the headers are sent: the answer comes without the body.
        const Reply reply = exchange(QHostAddress::LocalHost, port,
            request("POST", "/x", {hostHeader(port), "Content-Length: 104857601"}));
        QCOMPARE(reply.status, 413);
        QCOMPARE(server.server.bodiesInFlight(), 0);

        // Exactly the limit is accepted: the server waits for the body.
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(socket.waitForConnected(2000));
        socket.write(request("POST", "/x", {hostHeader(port), "Content-Length: 104857600"}));
        QTRY_COMPARE(server.server.bodiesInFlight(), 1);
    }

    // R29
    void idleTimeoutClosesAnIncompleteRequest()
    {
        HttpServerOptions options = testOptions();
        options.idleTimeoutMs = 300;
        TestServer server(options);
        const quint16 port = server.port();
        QTcpSocket stalled;
        stalled.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(stalled.waitForConnected(2000));
        stalled.write("GET / HTTP/1.1\r\nHost: 127.0.0.1");
        QTRY_COMPARE(server.server.openConnections(), 1);
        // Another connection is served meanwhile.
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port)})).status, 200);
        QTRY_COMPARE_WITH_TIMEOUT(server.server.openConnections(), 0, 3000);
        QTRY_COMPARE(stalled.state(), QAbstractSocket::UnconnectedState);
    }

    // R29
    void idleTimeoutClosesAClientThatStopsReading()
    {
        HttpServerOptions options = testOptions();
        options.idleTimeoutMs = 300;
        TestServer server(options, [](const HttpRequest &, HttpResponder respond) {
            HttpResponse response;
            response.body = QByteArray(64 * 1024 * 1024, 'x');
            respond(std::move(response));
        });
        const quint16 port = server.port();
        QTcpSocket client;
        client.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        client.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(client.waitForConnected(2000));
        client.write(request("GET", "/", {hostHeader(port)}));
        QTRY_COMPARE(server.server.openConnections(), 1);
        // The client never reads: the connection is closed instead of holding its slot.
        QTRY_COMPARE_WITH_TIMEOUT(server.server.openConnections(), 0, 5000);
    }

    // R30
    void headerOverTheLimitAnswers431()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        QCOMPARE(exchange(QHostAddress::LocalHost, port,
                     request("GET", "/", {hostHeader(port), "X-Padding: " + QByteArray(17 * 1024, 'a')})).status, 431);
        // No end of headers at all.
        QCOMPARE(exchange(QHostAddress::LocalHost, port, "GET / HTTP/1.1\r\nX: " + QByteArray(20 * 1024, 'a')).status, 431);
        // Just under the limit.
        QCOMPARE(exchange(QHostAddress::LocalHost, port,
                     request("GET", "/", {hostHeader(port), "X-Padding: " + QByteArray(16 * 1024 - 100, 'a')})).status, 200);
    }

    // R31
    void postWithoutContentLengthAnswers411()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("POST", "/x", {hostHeader(port)})).status, 411);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("POST", "/x", {hostHeader(port), "Content-Length: 0"})).status, 200);
    }

    // R32
    void transferEncodingAnswers501()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        QCOMPARE(exchange(QHostAddress::LocalHost, port,
                     request("POST", "/x", {hostHeader(port), "Transfer-Encoding: chunked"}, "0\r\n\r\n")).status, 501);
        QCOMPARE(exchange(QHostAddress::LocalHost, port,
                     request("GET", "/", {hostHeader(port), "Transfer-Encoding: chunked"})).status, 501);
    }

    // R33
    void seventeenthConnectionAnswers503()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        std::vector<std::unique_ptr<QTcpSocket>> held;
        for (int i = 0; i < 16; ++i) {
            held.push_back(std::make_unique<QTcpSocket>());
            held.back()->connectToHost(QHostAddress::LocalHost, port);
        }
        QTRY_COMPARE(server.server.openConnections(), 16);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port)})).status, 503);
        QCOMPARE(server.server.openConnections(), 16);
        held.pop_back();
        QTRY_COMPARE(server.server.openConnections(), 15);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port)})).status, 200);
    }

    // R34
    void thirdBodyInFlightAnswers503()
    {
        TestServer server(testOptions());
        const quint16 port = server.port();
        std::vector<std::unique_ptr<QTcpSocket>> senders;
        for (int i = 0; i < 2; ++i) {
            senders.push_back(std::make_unique<QTcpSocket>());
            senders.back()->connectToHost(QHostAddress::LocalHost, port);
            QVERIFY(senders.back()->waitForConnected(2000));
            senders.back()->write(request("POST", "/x", {hostHeader(port), "Content-Length: 1000"}, "abc"));
        }
        QTRY_COMPARE(server.server.bodiesInFlight(), 2);
        // The third POST is refused before its body is read: only its headers are sent.
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("POST", "/x", {hostHeader(port), "Content-Length: 1000"})).status, 503);
        // A GET is still served, and a slot frees up when a body completes.
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("GET", "/", {hostHeader(port)})).status, 200);
        senders.front()->write(QByteArray(997, 'x'));
        QTRY_COMPARE(server.server.bodiesInFlight(), 1);
        QCOMPARE(exchange(QHostAddress::LocalHost, port, request("POST", "/x", {hostHeader(port), "Content-Length: 2"}, "ab")).status, 200);
    }

    // R1, R2
    void textToImageListsModelsThatCanGenerate()
    {
        FakeDaemon daemon;
        daemon.setModels(categories(
            {imageModel("plain", "Plain model"),
             imageModel("generator", "Generator", {{"canGenerate", true}, {"canEdit", false}, {"width", 768}, {"height", 768}, {"steps", 20}, {"cfgScale", 5.5}}),
             imageModel("editor", "Editor only", {{"canGenerate", false}, {"canEdit", true}})},
            {QJsonObject{{"id", "video"}, {"name", "video"}, {"fileName", "video"}, {"displayName", "Video model"}}},
            {QJsonObject{{"id", "up"}, {"name", "up"}, {"fileName", "up"}, {"displayName", "Upscaler"}}}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        const QJsonArray actions = statusOf(port).value("actions").toArray();
        QCOMPARE(actions.size(), 1);
        const QJsonObject action = actions.at(0).toObject();
        QCOMPARE(action.value("id").toString(), QString("text-to-image"));
        QVERIFY(action.value("available").toBool());
        const QJsonArray models = action.value("models").toArray();
        QCOMPARE(models.size(), 2);
        QCOMPARE(models.at(0).toObject().value("id").toString(), QString("plain"));
        QCOMPARE(models.at(0).toObject().value("name").toString(), QString("Plain model"));
        QVERIFY(models.at(0).toObject().value("recommendedOptions").toObject().isEmpty());
        QCOMPARE(models.at(1).toObject().value("name").toString(), QString("Generator"));
        QCOMPARE(models.at(1).toObject().value("recommendedOptions").toObject().value("steps").toInt(), 20);
    }

    // R2
    void textToImageIsUnavailableWithoutAModelOrTheCapability()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("editor", "Editor only", {{"canGenerate", false}, {"canEdit", true}})}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        QJsonObject action = statusOf(port).value("actions").toArray().at(0).toObject();
        QVERIFY(!action.value("available").toBool());
        QCOMPARE(action.value("unavailable").toString(), QString("no-model"));
        QVERIFY(action.value("models").toArray().isEmpty());

        FakeDaemon incapable;
        incapable.setOperations({"image.upscale"});
        incapable.setModels(categories({imageModel("plain", "Plain model")}));
        QVERIFY(incapable.start());
        WebService other(testConfig(incapable.port()));
        QVERIFY2(other.start(&error), qPrintable(error));
        const quint16 otherPort = other.server()->port();
        QTRY_COMPARE(statusOf(otherPort).value("state").toString(), QString("available"));
        action = statusOf(otherPort).value("actions").toArray().at(0).toObject();
        QVERIFY(!action.value("available").toBool());
        QCOMPARE(action.value("unavailable").toString(), QString("unsupported-operation"));
        QCOMPARE(postJob(otherPort, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "a cat"}}).status, 400);
        QVERIFY(incapable.submits().isEmpty());
    }

    // R3
    void aModelInstalledLaterAppearsInTheNextStatus()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("first", "First")}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        QCOMPARE(statusOf(port).value("actions").toArray().at(0).toObject().value("models").toArray().size(), 1);
        // nukleusd announces no install: the model list is requested again on every status.
        daemon.setModels(categories({imageModel("first", "First"), imageModel("second", "Second")}));
        const int before = daemon.modelRequests();
        QCOMPARE(statusOf(port).value("actions").toArray().at(0).toObject().value("models").toArray().size(), 2);
        QVERIFY(daemon.modelRequests() > before);
    }

    // R4
    void submittedValuesAndPrefilledOptionsReachNukleus()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("plain", "Plain model"),
            imageModel("tuned", "Tuned", {{"width", 768}, {"height", 768}, {"steps", 20}, {"cfgScale", 5.5}})}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        const QJsonObject status = statusOf(port);
        QCOMPARE(status.value("defaults").toObject(), (QJsonObject{{"width", 1024}, {"height", 1024}, {"steps", 28}, {"cfgScale", 7}, {"seed", -1}}));
        QCOMPARE(status.value("actions").toArray().at(0).toObject().value("models").toArray().at(1).toObject().value("recommendedOptions").toObject().value("width").toInt(), 768);

        QCOMPARE(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "a cat"}, {"negativePrompt", "blur"},
                                {"width", "512"}, {"height", "640"}, {"steps", "12"}, {"cfgScale", "3.5"}, {"seed", "42"}}).status, 200);
        QTRY_COMPARE(daemon.submits().size(), 1);
        QJsonObject message = daemon.submits().at(0);
        QCOMPARE(message.value("operation").toString(), QString("image.generate"));
        QJsonObject options = message.value("options").toObject();
        QCOMPARE(options.value("modelId").toString(), QString("plain"));
        QCOMPARE(options.value("prompt").toString(), QString("a cat"));
        QCOMPARE(options.value("negativePrompt").toString(), QString("blur"));
        QCOMPARE(options.value("width").toInt(), 512);
        QCOMPARE(options.value("height").toInt(), 640);
        QCOMPARE(options.value("steps").toInt(), 12);
        QCOMPARE(options.value("cfgScale").toDouble(), 3.5);
        QCOMPARE(options.value("seed").toInt(), 42);

        // Fields left out: the model's recommended options, else the Nukleus defaults.
        QCOMPARE(postJob(port, {{"action", "text-to-image"}, {"modelId", "tuned"}, {"prompt", "a dog"}}).status, 200);
        QTRY_COMPARE(daemon.submits().size(), 2);
        options = daemon.submits().at(1).value("options").toObject();
        QCOMPARE(options.value("width").toInt(), 768);
        QCOMPARE(options.value("steps").toInt(), 20);
        QCOMPARE(options.value("cfgScale").toDouble(), 5.5);
        QCOMPARE(options.value("seed").toInt(), -1);
        QCOMPARE(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "a bird"}}).status, 200);
        QTRY_COMPARE(daemon.submits().size(), 3);
        options = daemon.submits().at(2).value("options").toObject();
        QCOMPARE(options.value("width").toInt(), 1024);
        QCOMPARE(options.value("height").toInt(), 1024);
        QCOMPARE(options.value("steps").toInt(), 28);
        QCOMPARE(options.value("cfgScale").toDouble(), 7.0);

        // A malformed number and an unknown model are refused before anything is sent.
        QCOMPARE(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "x"}, {"width", "wide"}}).status, 400);
        QCOMPARE(postJob(port, {{"action", "text-to-image"}, {"modelId", "missing"}, {"prompt", "x"}}).status, 400);
        QCOMPARE(daemon.submits().size(), 3);
    }

    // R9
    void aRejectedJobShowsNukleusError()
    {
        FakeDaemon daemon;
        daemon.setRejectSubmissions(true);
        daemon.setModels(categories({imageModel("plain", "Plain model")}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        const QString id = jsonOf(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "x"}, {"steps", "0"}})).value("id").toString();
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE(jobOf(port, id).value("state").toString(), QString("failed"));
        QCOMPARE(jobOf(port, id).value("error").toString(), QString("Invalid generation options"));
        QCOMPARE(getPath(port, "/api/jobs/" + id.toLatin1() + "/result").status, 409);
    }

    // R10
    void aJobGoesFromQueuedToRunningToFinished()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("plain", "Plain model")}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        const QString id = jsonOf(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "x"}})).value("id").toString();
        QCOMPARE(id.size(), 32);
        QCOMPARE(jobOf(port, id).value("state").toString(), QString("queued"));
        QTRY_COMPARE(daemon.submits().size(), 1);
        daemon.queuePosition(2);
        QTRY_COMPARE(jobOf(port, id).value("queuePosition").toInt(-1), 2);
        QCOMPARE(jobOf(port, id).value("state").toString(), QString("queued"));
        QCOMPARE(getPath(port, "/api/jobs/" + id.toLatin1() + "/result").status, 409);
        daemon.running();
        QTRY_COMPARE(jobOf(port, id).value("state").toString(), QString("running"));
        daemon.progress(40);
        QTRY_COMPARE(jobOf(port, id).value("progress").toInt(-1), 40);
        const QByteArray image("\x89PNG-fake-image");
        daemon.finish(image);
        QTRY_COMPARE(jobOf(port, id).value("state").toString(), QString("finished"));
        const Reply result = getPath(port, "/api/jobs/" + id.toLatin1() + "/result");
        QCOMPARE(result.status, 200);
        QCOMPARE(result.body, image);
        QCOMPARE(result.header("Content-Type"), QByteArray("image/png"));
        QVERIFY(result.header("Content-Disposition").startsWith("attachment; filename=\"nukleus-" + id.left(8).toLatin1()));
        QVERIFY(result.header("Content-Disposition").contains(".png"));

        const QString failing = jsonOf(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "y"}})).value("id").toString();
        QTRY_COMPARE(daemon.submits().size(), 2);
        daemon.running();
        daemon.fail("generation-failed", "Image generation failed");
        QTRY_COMPARE(jobOf(port, failing).value("state").toString(), QString("failed"));
        QCOMPARE(jobOf(port, failing).value("error").toString(), QString("Image generation failed"));
    }

    // R34
    void aPostIsRefusedAsBusyWhileTwoBodiesAreBeingReceived()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("plain", "Plain model")}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        std::vector<std::unique_ptr<QTcpSocket>> senders;
        for (int i = 0; i < 2; ++i) {
            senders.push_back(std::make_unique<QTcpSocket>());
            senders.back()->connectToHost(QHostAddress::LocalHost, port);
            QVERIFY(senders.back()->waitForConnected(2000));
            senders.back()->write(request("POST", "/api/jobs", {hostHeader(port), "Content-Type: multipart/form-data; boundary=XBOUNDARYX",
                                                                 "Content-Length: 5000"}, "--XBOUNDARYX"));
        }
        QTRY_COMPARE(service.server()->bodiesInFlight(), 2);
        // Refused before its body is read: only its headers are sent. The interface shows the 503 as busy.
        const Reply busy = exchange(QHostAddress::LocalHost, port, request("POST", "/api/jobs", {hostHeader(port),
            "Content-Type: multipart/form-data; boundary=XBOUNDARYX", "Content-Length: 5000"}));
        QCOMPARE(busy.status, 503);
        QVERIFY(busy.body.contains("busy"));
        QVERIFY(daemon.submits().isEmpty());
    }

    // R12, R13
    void jobsAreReachedOnlyByTheirIdentifierAndOnlyImageGenerateIsSubmitted()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("plain", "Plain model")}));
        QVERIFY(daemon.start());
        WebService service(testConfig(daemon.port()));
        QString error;
        QVERIFY2(service.start(&error), qPrintable(error));
        const quint16 port = service.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        const QString id = jsonOf(postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", "x"}})).value("id").toString();
        QVERIFY(id.size() >= 32); // 128 bits, hex
        QVERIFY(QRegularExpression("^[0-9a-f]+$").match(id).hasMatch());
        for (const QByteArray &path : {QByteArray("/api/jobs"), QByteArray("/api/jobs/"), QByteArray("/api/jobs/unknown"),
                                       QByteArray("/api/jobs/00000000000000000000000000000000"),
                                       QByteArray("/api/jobs/00000000000000000000000000000000/result")}) {
            const Reply reply = getPath(port, path);
            QCOMPARE(reply.status, 404);
            QVERIFY2(!reply.body.contains(id.toLatin1()), path.constData());
            QVERIFY2(!reply.body.contains("state"), path.constData());
        }
        // Other actions are not submitted, whatever their name.
        for (const QString &action : {QStringLiteral("image-to-image"), QStringLiteral("edit"), QStringLiteral("image-to-video"),
                                      QStringLiteral("text.chat"), QStringLiteral("image.upscale"), QString()})
            QCOMPARE(postJob(port, {{"action", action}, {"modelId", "plain"}, {"prompt", "x"}}).status, 400);
        QTRY_COMPARE(daemon.submits().size(), 1);
        for (const QJsonObject &submit : daemon.submits()) QCOMPARE(submit.value("operation").toString(), QString("image.generate"));
    }

    // R44, R45
    void theTwentyFirstJobMakesTheOldestUnavailableAndARestartForgetsAll()
    {
        FakeDaemon daemon;
        daemon.setModels(categories({imageModel("plain", "Plain model")}));
        QVERIFY(daemon.start());
        QStringList ids;
        {
            WebService service(testConfig(daemon.port()));
            QString error;
            QVERIFY2(service.start(&error), qPrintable(error));
            const quint16 port = service.server()->port();
            QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
            for (int i = 0; i < 21; ++i) {
                const Reply reply = postJob(port, {{"action", "text-to-image"}, {"modelId", "plain"}, {"prompt", QString::number(i)}});
                QCOMPARE(reply.status, 200);
                ids.append(jsonOf(reply).value("id").toString());
            }
            QCOMPARE(getPath(port, "/api/jobs/" + ids.first().toLatin1()).status, 404);
            QVERIFY(!getPath(port, "/api/jobs/" + ids.first().toLatin1()).body.contains("state"));
            QCOMPARE(getPath(port, "/api/jobs/" + ids.first().toLatin1() + "/result").status, 404);
            for (int i = 1; i < 21; ++i) QCOMPARE(getPath(port, "/api/jobs/" + ids.at(i).toLatin1()).status, 200);
        }
        WebService restarted(testConfig(daemon.port()));
        QString error;
        QVERIFY2(restarted.start(&error), qPrintable(error));
        const quint16 port = restarted.server()->port();
        QTRY_COMPARE(statusOf(port).value("state").toString(), QString("available"));
        for (const QString &id : ids) QCOMPARE(getPath(port, "/api/jobs/" + id.toLatin1()).status, 404);
    }

    // R26
    void acceptedHostValuesListTheNameForms()
    {
        const QStringList values = acceptedHostValues("Atlas.Home.Lan", QHostAddress("192.168.1.20"), 9385);
        QCOMPARE(values, (QStringList{"127.0.0.1:9385", "localhost:9385", "192.168.1.20:9385",
                                      "atlas.home.lan:9385", "atlas:9385", "atlas.local:9385"}));
        QCOMPARE(acceptedHostValues("atlas", {}, 9385).count(), 4);
    }
};

QTEST_MAIN(WebServiceTest)
#include "WebServiceTest.moc"
