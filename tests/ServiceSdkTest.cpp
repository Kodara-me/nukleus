#include <kira/service/ServiceClient.h>
#include <kira/service/Identity.h>
#include <archive.h>
#include <archive_entry.h>
#include <kira/service/ServiceProtocol.h>
#include <kira/service/ModelOptions.h>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

#include <algorithm>

namespace
{
class FakeSdkService final : public QObject
{
public:
    explicit FakeSdkService()
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
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"capabilities\",\"state\":\"ready\",\"operations\":[\"image.generate\",\"image.upscale\",\"image.edit\"]}"));
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list.result\",\"categories\":[{\"category\":\"image-generation\",\"models\":[{\"id\":\"image-generation:test.safetensors\",\"name\":\"Test\",\"fileName\":\"test.safetensors\",\"bytes\":1,\"modelType\":\"diffusion\",\"explicitContent\":true,\"companionFiles\":{\"clip-l\":\"companions/test.safetensors/clip-l/clip_l.safetensors\"},\"companionFileName\":\"companions/test.safetensors/vae.safetensors\",\"companionRole\":\"vae\",\"displayName\":\"Test\"}]},{\"category\":\"upscaler\",\"models\":[{\"id\":\"upscaler:test.gguf\",\"name\":\"Test 2x\",\"fileName\":\"test.gguf\",\"bytes\":2,\"displayName\":\"Test 2x\"}]},{\"category\":\"llm-chat\",\"models\":[{\"id\":\"llm-chat:test.gguf\",\"name\":\"Test LLM\",\"fileName\":\"llm.gguf\",\"bytes\":3,\"companionFileName\":\"companions/llm.gguf/mmproj.gguf\",\"companionRole\":\"mmproj\",\"displayName\":\"Test LLM\"}]}]}"));
                } else if (type == "models.configure") {
                    const QJsonObject reply{{"type", "models.configure.completed"}, {"id", message.value("id")}, {"model", QJsonObject{{"recommendedOptions", message.value("recommendedOptions")}}}};
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
                } else if (type == "models.install") {
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.install.accepted"}, {"installId", message.value("installId")}}).toJson(QJsonDocument::Compact)));
                } else if (type == "models.install.cancel") {
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.install.canceled"}, {"installId", message.value("installId")}}).toJson(QJsonDocument::Compact)));
                } else if (type == QStringLiteral("submit")) {
                    const QString jobId = message.value(QStringLiteral("jobId")).toString();
                    const auto options = message.value("options").toObject();
                    if (options.value("hasInput").toBool()) {
                        m_uploadJobs.insert(socket, jobId);
                        socket->sendTextMessage(QStringLiteral("{\"type\":\"upload-required\",\"jobId\":\"%1\"}").arg(jobId));
                    } else {
                        complete(socket, jobId, QByteArray("sdk-result"));
                    }
                }
            });
            connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray &data) {
                m_uploadedImages.append(data);
                const QString jobId = m_uploadJobs.take(socket);
                complete(socket, jobId, data.isEmpty() ? QByteArray("sdk-result") : QByteArray("sdk-result"));
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }
    QString errorString() const { return m_server.errorString(); }
    QUrl endpoint() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_server.serverPort())); }
    const QList<QJsonObject> &messages() const { return m_messages; }
    const QList<QByteArray> &uploadedImages() const { return m_uploadedImages; }

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
    QList<QByteArray> m_uploadedImages;
};
}

class ServiceSdkTest final : public QObject
{
    Q_OBJECT
private slots:
    void permanentQualityRoutingValidation()
    {
        auto routing = KiraService::Protocol::starterModelRouting();
        routing.editingQualities[0].permanent = true;
        routing.presets[0].qualities[1].permanent = true;
        routing.presets[1].qualities[2].permanent = true;
        const auto json = KiraService::Protocol::modelRoutingToJson(routing);
        QCOMPARE(KiraService::Protocol::modelRoutingFromJson(json), std::optional(routing));
        routing.editingQualities[1].permanent = true;
        QVERIFY(!KiraService::Protocol::validateModelRouting(routing));
        QVERIFY(!KiraService::Protocol::modelRoutingFromJson(KiraService::Protocol::modelRoutingToJson(routing)));
        routing.editingQualities[1].permanent = false;
        routing.presets[0].qualities[0].permanent = true;
        QVERIFY(!KiraService::Protocol::validateModelRouting(routing));
        auto oldJson = KiraService::Protocol::modelRoutingToJson(KiraService::Protocol::starterModelRouting());
        auto editing = oldJson.value("editingQualities").toArray();
        auto quality = editing[0].toObject();
        quality.remove("permanent"); editing[0] = quality;
        oldJson.insert("editingQualities", editing);
        QVERIFY(KiraService::Protocol::modelRoutingFromJson(oldJson));
        quality.insert("permanent", "true"); editing[0] = quality;
        oldJson.insert("editingQualities", editing);
        QVERIFY(!KiraService::Protocol::modelRoutingFromJson(oldJson));
    }
    void validatesPromptStyles();
    void configuresAndCancelsModels();
    void preservesProtocolV3();
    void authenticatesAndDiscoversModels();
    void submitsBinaryInputAndReturnsBinaryOutput();
    void supportsMultipleJobs();
    void installsModelWithCompanion();
    void installsModularCompanionsForExistingModel();
    void reportsConnectionFailure();
    void identityPackages();
};

void ServiceSdkTest::validatesPromptStyles()
{
    using namespace KiraService::Protocol;
    QString error;
    const auto starters = starterPromptStyles();
    QCOMPARE(promptStylesFromJson(promptStylesToJson(starters)).value(), starters);
    QVERIFY(promptStylesFromJson(QJsonArray{})->isEmpty());
    QVERIFY(!promptStylesFromJson(QJsonObject{}, &error));
    QVERIFY(!promptStylesFromJson(QJsonArray{42}, &error));
    const auto valid = promptStylesToJson(starters).first().toObject();
    for (const auto &field : {"id", "name", "positivePrefix", "negativePrefix"}) {
        auto invalid = valid; invalid.remove(field);
        QVERIFY(!promptStylesFromJson(QJsonArray{invalid}, &error));
        invalid = valid; invalid.insert(field, true);
        QVERIFY(!promptStylesFromJson(QJsonArray{invalid}, &error));
    }
    for (const auto &field : {"id", "name"}) {
        auto invalid = valid; invalid.insert(field, " ");
        QVERIFY(!promptStylesFromJson(QJsonArray{invalid}, &error));
    }
    QVERIFY(!validatePromptStyles({{"id", "Name", " ", "\n"}}, &error));
    QVERIFY(!validatePromptStyles({starters.first(), starters.first()}, &error));
    QVERIFY(validatePromptStyles({{"id", "Name", "", "negative"}}, &error));
    QCOMPARE(composeStyledPrompt("  prefix \n", "  prompt  "), QString("prefix, prompt"));
    QCOMPARE(composeStyledPrompt("", "  prompt  "), QString("prompt"));
    QCOMPARE(composeStyledPrompt("negative", ""), QString("negative"));
}

void ServiceSdkTest::identityPackages()
{
    const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a") + QByteArray(2000, 'x');
    KiraService::IdentityPackage original;
    original.references = {png, png + "2", png + "3", png + "4"};
    original.identityPrompt = "adult with green eyes";
    original.hairPrompt = "curly brown hair";
    original.seed = 123;
    QString error;
    const auto package = KiraService::packIdentity(original, &error);
    QVERIFY2(!package.isEmpty(), qPrintable(error));
    QVERIFY(package.startsWith("PK"));
    KiraService::IdentityPackage decoded;
    QVERIFY2(KiraService::unpackIdentity(package, decoded, &error), qPrintable(error));
    QCOMPARE(decoded.references, original.references);
    QCOMPARE(decoded.identityPrompt, original.identityPrompt);
    QCOMPARE(decoded.hairPrompt, original.hairPrompt);
    QCOMPARE(decoded.seed, original.seed);
    // Verify the exported archive contains only PNG references and its manifest.
    auto* reader = archive_read_new();
    archive_read_support_format_zip(reader);
    QCOMPARE(archive_read_open_memory(reader, package.constData(), package.size()), ARCHIVE_OK);
    QStringList names;
    archive_entry* entry = nullptr;
    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
        names.append(QString::fromUtf8(archive_entry_pathname(entry)));
        archive_read_data_skip(reader);
    }
    archive_read_free(reader);
    names.sort();
    QCOMPARE(names, QStringList({"manifest.json", "reference-2.png", "reference-3.png", "reference-4.png", "reference.png"}));
    QVERIFY(!KiraService::unpackIdentity("not a zip", decoded, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(decoded.references.isEmpty());
    QVERIFY(!KiraService::unpackIdentity(package.left(package.size() / 2), decoded));
    QVERIFY(KiraService::packIdentity({}).isEmpty());
    original.references = {"not a png"};
    QVERIFY(KiraService::packIdentity(original).isEmpty());
    original.references = {png, png, png, png, png};
    QVERIFY(KiraService::packIdentity(original).isEmpty());
}

void ServiceSdkTest::configuresAndCancelsModels()
{
    FakeSdkService service; QVERIFY(service.start()); KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService(); QTRY_VERIFY(client.modelListAvailable());
    QCOMPARE(client.models(KiraService::ModelCategory::Upscaler).first().recommendedOptions, QJsonObject{});
    QSignalSpy configured(&client, &KiraService::ServiceClient::modelConfigured);
    QVERIFY(!client.configureModel("upscaler:test.gguf", {}, {}, QString("  ")));
    QVERIFY(client.installModel(KiraService::ModelCategory::LlmChat, QUrl("https://example.test/model.gguf")).isEmpty());
    QVERIFY(client.configureModel("upscaler:test.gguf", {}, {}, QString("Upscaler"))); QTRY_COMPARE(configured.count(), 1); QCOMPARE(configured.first().at(1).value<QJsonObject>(), QJsonObject{});
    const auto options = KiraService::Protocol::defaultModelOptions("llm-chat");
    const auto id = client.installModel(KiraService::ModelCategory::LlmChat, QUrl("https://example.test/model.gguf"), {}, options, {}, "checkpoint", {}, true, QString("Chat")); QVERIFY(!id.isEmpty());
    QTRY_COMPARE(client.installs().last().state, KiraService::ModelInstallState::Accepted);
    const auto installMessage = std::find_if(service.messages().cbegin(), service.messages().cend(), [](const QJsonObject &message) { return message.value("type") == "models.install"; });
    QVERIFY(installMessage != service.messages().cend()); QCOMPARE(installMessage->value("recommendedOptions").toObject(), options);
    QVERIFY(installMessage->value("explicitContent").toBool());
    QVERIFY(client.cancelInstall(id)); QTRY_COMPARE(client.installs().last().state, KiraService::ModelInstallState::Canceled);
    client.disconnectFromService(); QTRY_VERIFY(!client.isAuthenticated()); QCOMPARE(client.installs().last().state, KiraService::ModelInstallState::Canceled);
}

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
    QVERIFY(client.canSubmit(KiraService::ImageOperation::Edit, "image-generation:test.safetensors"));
    QVERIFY(client.models(KiraService::ModelCategory::ImageGeneration).first().explicitContent);
    QCOMPARE(client.models(KiraService::ModelCategory::LlmChat).first().companionRole, QStringLiteral("mmproj"));
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).first().modelType, QString("diffusion"));
    QVERIFY(client.models(KiraService::ModelCategory::ImageGeneration).first().styles.isEmpty());
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).first().companionFiles.value("clip-l").toString(), QString("companions/test.safetensors/clip-l/clip_l.safetensors"));
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).first().companionFileName, QStringLiteral("companions/test.safetensors/vae.safetensors"));
    QCOMPARE(client.models(KiraService::ModelCategory::Upscaler).size(), 1);
    QCOMPARE(client.models(KiraService::ModelCategory::LlmChat).size(), 1);
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

void ServiceSdkTest::installsModelWithCompanion()
{
    FakeSdkService service; QVERIFY(service.start());
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.modelListAvailable());
    const QUrl companion(QStringLiteral("https://example.test/vae.safetensors"));
    QVERIFY(!client.installModel(KiraService::ModelCategory::ImageGeneration, QUrl(QStringLiteral("https://example.test/model.gguf")), companion, {}, {}, "checkpoint", {}, {}, QString("Generator")).isEmpty());
    QTRY_VERIFY(std::any_of(service.messages().cbegin(), service.messages().cend(), [](const QJsonObject &message) {
        return message.value(QStringLiteral("type")).toString() == QStringLiteral("models.install");
    }));
    const auto message = service.messages().last();
    QCOMPARE(message.value(QStringLiteral("companionUrl")).toString(), companion.toString());
    QVERIFY(!message.contains(QStringLiteral("sha256")));
    QCOMPARE(client.installs().last().companionUrl, companion);
}

void ServiceSdkTest::installsModularCompanionsForExistingModel()
{
    FakeSdkService service;
    QVERIFY(service.start());
    KiraService::ServiceClient client;
    client.setConfiguration(service.endpoint(), {});
    client.connectToService();
    QTRY_VERIFY(client.modelListAvailable());
    const QJsonObject files{{"clip-l", "https://example.test/clip_l.safetensors"},
        {"clip-g", "https://example.test/clip_g.safetensors"}};
    const auto installId = client.installModel(KiraService::ModelCategory::ImageGeneration, {}, {}, {}, files,
        "diffusion", "image-generation:test.safetensors");
    QVERIFY(!installId.isEmpty());
    QTRY_VERIFY(std::any_of(service.messages().cbegin(), service.messages().cend(), [&](const auto &message) {
        return message.value("installId").toString() == installId;
    }));
    const auto message = service.messages().last();
    QCOMPARE(message.value("companionUrls").toObject(), files);
    QCOMPARE(message.value("modelType").toString(), QString("diffusion"));
    QCOMPARE(message.value("id").toString(), QString("image-generation:test.safetensors"));
    QVERIFY(!message.contains("recommendedOptions"));
}

void ServiceSdkTest::supportsMultipleJobs()
{
    FakeSdkService service; QVERIFY2(service.start(), qPrintable(service.errorString()));
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.modelListAvailable());
    QSignalSpy finished(&client, &KiraService::ServiceClient::imageJobFinished);
    KiraService::ImageGenerationRequest first; first.prompt = QStringLiteral("one"); first.modelId = QStringLiteral("image-generation:test.safetensors"); first.clipSkip = 2;
    KiraService::ImageGenerationRequest second = first; second.prompt = QStringLiteral("two");
    QTRY_VERIFY(client.canSubmit(KiraService::ImageOperation::Generate, first.modelId));
    QVERIFY(!client.submitGeneration(first).isEmpty());
    QVERIFY(!client.submitGeneration(second).isEmpty());
    QTRY_COMPARE(finished.count(), 2);
    const auto submitted = std::find_if(service.messages().cbegin(), service.messages().cend(), [](const QJsonObject &message) {
        return message.value(QStringLiteral("type")).toString() == QStringLiteral("submit")
               && message.value(QStringLiteral("options")).toObject().value(QStringLiteral("prompt")).toString() == QStringLiteral("one");
    });
    QVERIFY(submitted != service.messages().cend());
    QCOMPARE(submitted->value(QStringLiteral("options")).toObject().value(QStringLiteral("clipSkip")).toInt(), 2);
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
