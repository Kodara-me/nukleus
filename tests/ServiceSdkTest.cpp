#include <kira/service/ServiceClient.h>
#include <kira/service/ServiceProtocol.h>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

namespace
{
class FakeSdkService final : public QObject
{
public:
    FakeSdkService()
        : m_server(QStringLiteral("sdk-test"), QWebSocketServer::NonSecureMode, this)
    {
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            QWebSocket *socket = m_server.nextPendingConnection();
            connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &text) {
                const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
                m_messages.append(message);
                const QString type = message.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("authenticate")) {
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"authenticated\"}"));
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"capabilities\",\"state\":\"ready\",\"operations\":[\"image.generate\",\"image.upscale\"]}"));
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list.result\",\"categories\":[{\"category\":\"image-generation\",\"models\":[{\"id\":\"image-generation:test.safetensors\",\"name\":\"Test\",\"fileName\":\"test.safetensors\",\"bytes\":1}]},{\"category\":\"upscaler\",\"models\":[{\"id\":\"upscaler:test.gguf\",\"name\":\"Test 2x\",\"fileName\":\"test.gguf\",\"bytes\":2}]},{\"category\":\"llm-chat\",\"models\":[{\"id\":\"llm-chat:test.gguf\",\"name\":\"Test LLM\",\"fileName\":\"llm.gguf\",\"bytes\":3}]},{\"category\":\"ip-adapter\",\"models\":[{\"id\":\"ip-adapter:test.bin\",\"name\":\"Test Adapter\",\"fileName\":\"test.bin\",\"bytes\":4,\"companionFileName\":\"companions/test.bin/test-lora.safetensors\"}]}]}"));
                } else if (type == QStringLiteral("submit")) {
                    const QString jobId = message.value(QStringLiteral("jobId")).toString();
                    if (message.value(QStringLiteral("options")).toObject().value(QStringLiteral("hasInput")).toBool()) {
                        m_uploadJobs.insert(socket, jobId);
                        socket->sendTextMessage(QStringLiteral("{\"type\":\"upload-required\",\"jobId\":\"%1\"}").arg(jobId));
                    } else {
                        complete(socket, jobId, QByteArray("sdk-result"));
                    }
                }
            });
            connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray &data) {
                const QString jobId = m_uploadJobs.take(socket);
                complete(socket, jobId, data.isEmpty() ? QByteArray("sdk-result") : QByteArray("sdk-result"));
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }
    QString errorString() const { return m_server.errorString(); }
    QUrl endpoint() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_server.serverPort())); }
    const QList<QJsonObject> &messages() const { return m_messages; }

private:
    void complete(QWebSocket *socket, const QString &jobId, const QByteArray &data)
    {
        if (jobId.isEmpty()) return;
        socket->sendTextMessage(QStringLiteral("{\"type\":\"queued\",\"jobId\":\"%1\"}").arg(jobId));
        socket->sendTextMessage(QStringLiteral("{\"type\":\"running\",\"jobId\":\"%1\"}").arg(jobId));
        socket->sendTextMessage(QStringLiteral("{\"type\":\"progress\",\"jobId\":\"%1\",\"progress\":{\"stage\":\"sampling\",\"percentage\":50,\"statusText\":\"Sampling\"}}").arg(jobId));
        socket->sendTextMessage(QStringLiteral("{\"type\":\"result\",\"jobId\":\"%1\",\"mimeType\":\"image/png\"}").arg(jobId));
        socket->sendBinaryMessage(data);
    }

    QWebSocketServer m_server;
    QList<QJsonObject> m_messages;
    QHash<QWebSocket *, QString> m_uploadJobs;
};
}

class ServiceSdkTest final : public QObject
{
    Q_OBJECT
private slots:
    void preservesProtocolV3();
    void authenticatesAndDiscoversModels();
    void submitsBinaryInputAndReturnsBinaryOutput();
    void supportsMultipleJobs();
    void reportsConnectionFailure();
};

void ServiceSdkTest::preservesProtocolV3()
{
    QCOMPARE(KiraService::Protocol::Version, 3);
    QCOMPARE(KiraService::Protocol::DefaultPort, quint16(9384));
}

void ServiceSdkTest::authenticatesAndDiscoversModels()
{
    FakeSdkService service; QVERIFY2(service.start(), qPrintable(service.errorString()));
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.isAuthenticated());
    QTRY_VERIFY(client.modelListAvailable());
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).size(), 1);
    QCOMPARE(client.models(KiraService::ModelCategory::Upscaler).size(), 1);
    QCOMPARE(client.models(KiraService::ModelCategory::LlmChat).size(), 1);
    const QList<KiraService::ModelInfo> ipAdapters = client.models(KiraService::ModelCategory::IpAdapter);
    QCOMPARE(ipAdapters.size(), 1);
    QCOMPARE(ipAdapters.first().companionFileName, QStringLiteral("companions/test.bin/test-lora.safetensors"));
}

void ServiceSdkTest::submitsBinaryInputAndReturnsBinaryOutput()
{
    FakeSdkService service; QVERIFY2(service.start(), qPrintable(service.errorString()));
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.modelListAvailable());
    QSignalSpy finished(&client, &KiraService::ServiceClient::imageJobFinished);
    KiraService::ImageUpscaleRequest request{QByteArray("source-bytes"), QStringLiteral("upscaler:test.gguf")};
    QTRY_VERIFY(client.canSubmit(KiraService::ImageOperation::Upscale, request.modelId));
    const QString jobId = client.submitUpscale(request);
    QVERIFY(!jobId.isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    const auto result = finished.takeFirst().at(0).value<KiraService::ImageJobResult>();
    QVERIFY(result.succeeded);
    QCOMPARE(result.imageData, QByteArray("sdk-result"));
    for (const QJsonObject &message : service.messages()) QVERIFY(!QJsonDocument(message).toJson().contains("source-bytes"));
}

void ServiceSdkTest::supportsMultipleJobs()
{
    FakeSdkService service; QVERIFY2(service.start(), qPrintable(service.errorString()));
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.modelListAvailable());
    QSignalSpy finished(&client, &KiraService::ServiceClient::imageJobFinished);
    KiraService::ImageGenerationRequest first; first.prompt = QStringLiteral("one"); first.modelId = QStringLiteral("image-generation:test.safetensors");
    KiraService::ImageGenerationRequest second = first; second.prompt = QStringLiteral("two");
    QTRY_VERIFY(client.canSubmit(KiraService::ImageOperation::Generate, first.modelId));
    QVERIFY(!client.submitGeneration(first).isEmpty());
    QVERIFY(!client.submitGeneration(second).isEmpty());
    QTRY_COMPARE(finished.count(), 2);
}

void ServiceSdkTest::reportsConnectionFailure()
{
    KiraService::ServiceClient client; QSignalSpy states(&client, &KiraService::ServiceClient::serviceStateChanged);
    client.setConfiguration(QUrl(QStringLiteral("ws://127.0.0.1:1")), {}); client.connectToService();
    QTRY_VERIFY_WITH_TIMEOUT(states.count() >= 2, 2000);
    QVERIFY(states.last().at(0).value<KiraService::ServiceConnectionState>() == KiraService::ServiceConnectionState::Disconnected
            || states.last().at(0).value<KiraService::ServiceConnectionState>() == KiraService::ServiceConnectionState::Error);
}

QTEST_MAIN(ServiceSdkTest)
#include "ServiceSdkTest.moc"
