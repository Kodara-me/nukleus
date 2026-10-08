// Minimal stand-in for llama-server used by ServiceTest. It records its launches and the last
// chat request in KIRA_FAKE_LLAMA_DIR, and changes behavior when marker files exist there.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
QDir directory() { return QDir(qEnvironmentVariable("KIRA_FAKE_LLAMA_DIR")); }
bool marker(const QString &name) { return directory().exists(name); }

void append(const QString &name, const QByteArray &bytes)
{
    QFile file(directory().filePath(name));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(bytes);
}

void write(const QString &name, const QByteArray &bytes)
{
    QFile file(directory().filePath(name));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(bytes);
}

void respond(QTcpSocket *socket, int status, const QJsonObject &body)
{
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fake\r\nContent-Type: application/json\r\nContent-Length: "
                  + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
    socket->disconnectFromHost();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments().mid(1);
    if (arguments == QStringList{"--help"}) {
        QTextStream(stdout) << "--chat-template JINJA_TEMPLATE\n    list of built-in templates:\n"
                               "    chatml, mistral-v7,\n    zephyr\n    (env: LLAMA_ARG_CHAT_TEMPLATE)\n";
        return 0;
    }
    append("launches", arguments.join(QLatin1Char(' ')).toUtf8() + '\n');
    write("pid", QByteArray::number(QCoreApplication::applicationPid()));
    if (marker("load-fail")) {
        std::fprintf(stderr, "failed to allocate model buffer: out of memory\n");
        return 1;
    }
    QString apiKey;
    quint16 port = 0;
    for (int i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments.at(i) == "--port") port = static_cast<quint16>(arguments.at(i + 1).toUInt());
        if (arguments.at(i) == "--api-key-file") {
            QFile key(arguments.at(i + 1));
            if (key.open(QIODevice::ReadOnly)) apiKey = QString::fromUtf8(key.readAll()).trimmed();
        }
    }
    QElapsedTimer started;
    started.start();
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, port)) {
        std::fprintf(stderr, "couldn't bind HTTP server socket\n");
        return 1;
    }
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
            *buffer += socket->readAll();
            const auto headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            const QByteArray head = buffer->left(headerEnd);
            qsizetype length = 0;
            for (const QByteArray &line : head.split('\n'))
                if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
            if (buffer->size() < headerEnd + 4 + length) return;
            const QByteArray body = buffer->mid(headerEnd + 4, length);
            if (head.startsWith("GET /health")) {
                const bool loading = marker("slow-load") && started.elapsed() < 300;
                respond(socket, loading ? 503 : 200, {{"status", loading ? "loading model" : "ok"}});
                return;
            }
            if (!head.contains("Authorization: Bearer " + apiKey.toUtf8())) {
                respond(socket, 401, {{"error", QJsonObject{{"message", "Invalid API Key"}, {"type", "authentication_error"}}}});
                return;
            }
            write("request.json", body);
            if (marker("crash")) {
                std::fprintf(stderr, "GGML_ASSERT failed\n");
                std::exit(3);
            }
            if (marker("hang")) return;
            if (marker("context")) {
                respond(socket, 400, {{"error", QJsonObject{{"code", 400}, {"message", "the request exceeds the available context size"},
                    {"type", "exceed_context_size_error"}, {"n_prompt_tokens", 5000}, {"n_ctx", 4096}}}});
                return;
            }
            // A JSON schema yields a JSON reply; "truncated" simulates one cut by max_tokens.
            const bool structured = QJsonDocument::fromJson(body).object().contains("response_format");
            const bool truncated = marker("truncated");
            const QString content = truncated ? "{\"speech\":\"*smi"
                : structured ? "{\"speech\":\"*smiles* Hello\"}" : "<think>x</think> *smiles* Hello\nUser: hi";
            respond(socket, 200, {{"choices", QJsonArray{QJsonObject{{"index", 0}, {"finish_reason", truncated ? "length" : "stop"},
                {"message", QJsonObject{{"role", "assistant"}, {"content", content}}}}}},
                {"usage", QJsonObject{{"prompt_tokens", 42}, {"completion_tokens", 7}, {"total_tokens", 49}}}});
        });
    });
    QTextStream(stdout) << "fake llama-server listening\n" << Qt::flush;
    return application.exec();
}
