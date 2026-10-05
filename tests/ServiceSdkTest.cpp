#include <kira/service/ServiceClient.h>
#include <kira/service/PhotoMakerIdentity.h>
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
    explicit FakeSdkService(bool ipAdapter = false)
        : m_server(QStringLiteral("sdk-test"), QWebSocketServer::NonSecureMode, this), m_ipAdapter(ipAdapter)
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
                    socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list.result\",\"categories\":[{\"category\":\"image-generation\",\"models\":[{\"id\":\"image-generation:test.safetensors\",\"name\":\"Test\",\"fileName\":\"test.safetensors\",\"bytes\":1,\"modelType\":\"diffusion\",\"explicitContent\":true,\"companionFiles\":{\"clip-l\":\"companions/test.safetensors/clip-l/clip_l.safetensors\"},\"companionFileName\":\"companions/test.safetensors/vae.safetensors\",\"companionRole\":\"vae\"}]},{\"category\":\"edition\",\"models\":[{\"id\":\"edition:edit.safetensors\",\"name\":\"Edit\",\"fileName\":\"edit.safetensors\",\"bytes\":5,\"companionFileName\":\"companions/edit.safetensors/vae.gguf\",\"companionRole\":\"vae\"}]},{\"category\":\"upscaler\",\"models\":[{\"id\":\"upscaler:test.gguf\",\"name\":\"Test 2x\",\"recommendedOptions\":{\"tileSize\":128},\"fileName\":\"test.gguf\",\"bytes\":2}]},{\"category\":\"llm-chat\",\"models\":[{\"id\":\"llm-chat:test.gguf\",\"name\":\"Test LLM\",\"fileName\":\"llm.gguf\",\"bytes\":3,\"companionFileName\":\"companions/llm.gguf/mmproj.gguf\",\"companionRole\":\"mmproj\"}]},{\"category\":\"ip-adapter\",\"models\":[{\"id\":\"ip-adapter:test.bin\",\"name\":\"Test Adapter\",\"fileName\":\"test.bin\",\"bytes\":4,\"companionFileName\":\"companions/test.bin/test-lora.safetensors\"}]}]}"));
                    if (m_ipAdapter) {
                        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                            {"type", "capabilities"}, {"state", "ready"}, {"ipAdapter", true}, {"photoMaker", true},
                            {"operations", QJsonArray{"image.generate", "image.upscale", "image.edit"}}}).toJson()));
                        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                            {"type", "models.list.result"}, {"categories", QJsonArray{
                                QJsonObject{{"category", "image-generation"}, {"models", QJsonArray{QJsonObject{{"id", "image-generation:test.safetensors"}}}}},
                                QJsonObject{{"category", "edition"}, {"models", QJsonArray{QJsonObject{{"id", "edition:edit.safetensors"}}}}},
                                QJsonObject{{"category", "clip_vision"}, {"models", QJsonArray{QJsonObject{{"id", "clip_vision:vision.safetensors"}}}}},
                                QJsonObject{{"category", "ip_adapter"}, {"models", QJsonArray{QJsonObject{{"id", "ip-adapter:test.bin"}}}}},
                                QJsonObject{{"category", "photomaker"}, {"models", QJsonArray{QJsonObject{{"id", "photomaker:test.safetensors"}}}}}}}}).toJson()));
                    }
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
                    if (options.value("hasPhotoMakerIdentity").toBool()) {
                        m_uploadJobs.insert(socket, jobId);
                        m_photoMakerUploads.insert(socket, options.value("referenceCount").toInt(1) + 1);
                        m_referenceCounts.insert(socket, options.value("referenceCount").toInt(1));
                        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                            {"type", "upload-required"}, {"jobId", jobId}, {"inputRole", "photomaker-reference"}}).toJson()));
                        return;
                    }
                    if (options.value("hasInput").toBool() || options.value("hasIpAdapterImage").toBool()) {
                        m_uploadJobs.insert(socket, jobId);
                        if (options.value("hasIpAdapterImage").toBool()) {
                            m_adapterUploads.insert(socket, options.value("hasInput").toBool() ? 2 : 1);
                            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                                {"type", "upload-required"}, {"jobId", jobId},
                                {"inputRole", options.value("hasInput").toBool() ? "source" : "ip-adapter"}}).toJson()));
                            return;
                        }
                        socket->sendTextMessage(QStringLiteral("{\"type\":\"upload-required\",\"jobId\":\"%1\"}").arg(jobId));
                    } else {
                        complete(socket, jobId, QByteArray("sdk-result"));
                    }
                }
            });
            connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray &data) {
                m_uploadedImages.append(data);
                if (m_photoMakerUploads.value(socket) > 2) {
                    --m_photoMakerUploads[socket];
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                        {"type", "upload-required"}, {"jobId", m_uploadJobs.value(socket)},
                        {"inputRole", "photomaker-reference"},
                        {"referenceIndex", m_referenceCounts.value(socket) - m_photoMakerUploads.value(socket) + 1}}).toJson()));
                    return;
                }
                if (m_photoMakerUploads.value(socket) == 2) {
                    m_photoMakerUploads[socket] = 1;
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                        {"type", "upload-required"}, {"jobId", m_uploadJobs.value(socket)}, {"inputRole", "photomaker-identity"}}).toJson()));
                    return;
                }
                m_photoMakerUploads.remove(socket);
                if (m_adapterUploads.value(socket) == 2) {
                    m_adapterUploads[socket] = 1;
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                        {"type", "upload-required"}, {"jobId", m_uploadJobs.value(socket)}, {"inputRole", "ip-adapter"}}).toJson()));
                    return;
                }
                m_adapterUploads.remove(socket);
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
    QHash<QWebSocket *, int> m_adapterUploads;
    QList<QByteArray> m_uploadedImages;
    bool m_ipAdapter = false;
    QHash<QWebSocket *, int> m_photoMakerUploads;
    QHash<QWebSocket *, int> m_referenceCounts;
};
}

class ServiceSdkTest final : public QObject
{
    Q_OBJECT
private slots:
    void validatesPromptStyles();
    void configuresAndCancelsModels();
    void preservesProtocolV3();
    void authenticatesAndDiscoversModels();
    void submitsBinaryInputAndReturnsBinaryOutput();
    void supportsMultipleJobs();
    void installsModelWithCompanion();
    void installsModularCompanionsForExistingModel();
    void reportsConnectionFailure();
    void submitsIpAdapterReferences();
    void submitsPhotoMakerIdentity();
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
    const QByteArray bin("embedding\0bytes", 15);
    QString error;
    const auto package = KiraService::packPhotoMakerIdentity(png, bin, &error);
    KiraService::PhotoMakerIdentityPackage full;
    full.references = {png, png + "2", png + "3", png + "4"};
    full.embeddings = bin;
    full.identityPrompt = "adult with green eyes";
    full.hairPrompt = "curly brown hair";
    full.seed = 123;
    const auto multi = KiraService::packPhotoMakerIdentity(full, &error);
    QVERIFY2(!multi.isEmpty(), qPrintable(error));
    KiraService::PhotoMakerIdentityPackage decoded;
    QVERIFY2(KiraService::unpackPhotoMakerIdentity(multi, decoded, &error), qPrintable(error));
    QCOMPARE(decoded.references, full.references);
    QCOMPARE(decoded.embeddings, bin);
    QCOMPARE(decoded.identityPrompt, full.identityPrompt);
    QCOMPARE(decoded.hairPrompt, full.hairPrompt);
    QCOMPARE(decoded.seed, 123);
    QVERIFY2(!package.isEmpty(), qPrintable(error));
    QVERIFY(package.startsWith("PK"));
    QVERIFY(package.size() < png.size());
    QByteArray reference, identity;
    QVERIFY2(KiraService::unpackPhotoMakerIdentity(package, reference, identity, &error), qPrintable(error));
    QCOMPARE(reference, png); QCOMPARE(identity, bin);
    QVERIFY(!KiraService::unpackPhotoMakerIdentity("not a zip", reference, identity, &error));
    QVERIFY(!error.isEmpty()); QVERIFY(reference.isEmpty()); QVERIFY(identity.isEmpty());
    QVERIFY(!KiraService::unpackPhotoMakerIdentity(package.left(package.size() / 2), reference, identity));
    QVERIFY(KiraService::packPhotoMakerIdentity(png, {}).isEmpty());
    QVERIFY(KiraService::packPhotoMakerIdentity("not png", bin).isEmpty());
    // Independently authored ZIPs exercise strict entry/manifest validation.
    const auto customZip = [](const QList<QPair<QByteArray, QByteArray>>& files) {
        QByteArray output(8192, Qt::Uninitialized);
        size_t used = 0;
        auto* writer = archive_write_new();
        archive_write_set_format_zip(writer);
        archive_write_open_memory(writer, output.data(), output.size(), &used);
        for (const auto& file : files) {
            auto* entry = archive_entry_new();
            archive_entry_set_pathname(entry, file.first.constData());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0600);
            archive_entry_set_size(entry, file.second.size());
            archive_write_header(writer, entry);
            archive_write_data(writer, file.second.constData(), file.second.size());
            archive_entry_free(entry);
        }
        archive_write_close(writer); archive_write_free(writer);
        output.resize(used); return output;
    };
    QList<QPair<QByteArray, QByteArray>> files{
        {"reference.png", png}, {"id_embeds.bin", bin},
        {"manifest.json", "{\"type\":\"photomaker-v2\",\"version\":1}"}};
    QVERIFY(KiraService::unpackPhotoMakerIdentity(customZip(files), reference, identity));
    auto bad = files; bad.removeLast();
    QVERIFY(!KiraService::unpackPhotoMakerIdentity(customZip(bad), reference, identity));
    bad = files; bad.append(files.first());
    QVERIFY(!KiraService::unpackPhotoMakerIdentity(customZip(bad), reference, identity));
    bad = files; bad.first().first = "../reference.png";
    QVERIFY(!KiraService::unpackPhotoMakerIdentity(customZip(bad), reference, identity));
    bad = files; bad.last().second = "{\"type\":\"photomaker-v2\",\"version\":2}";
    QVERIFY(!KiraService::unpackPhotoMakerIdentity(customZip(bad), reference, identity));
}

void ServiceSdkTest::submitsPhotoMakerIdentity()
{
    FakeSdkService service(true); QVERIFY(service.start());
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.capabilities().photoMaker);
    QTRY_COMPARE(client.models(KiraService::ModelCategory::PhotoMaker).size(), 1);
    KiraService::ImageGenerationRequest request;
    request.modelId = "image-generation:test.safetensors"; request.prompt = "a woman img";
    request.styleId = "cinematic";
    request.photoMakerModelId = "photomaker:test.safetensors";
    request.photoMakerReferenceImageData = "reference";
    request.photoMakerIdentityData = QByteArray("identity\0bytes", 14);
    request.photoMakerStyleStrength = 17;
    QSignalSpy results(&client, &KiraService::ServiceClient::imageJobFinished);
    QVERIFY(!client.submitGeneration(request).isEmpty()); QTRY_COMPARE(results.count(), 1);
    QCOMPARE(service.uploadedImages(), QList<QByteArray>({request.photoMakerReferenceImageData, request.photoMakerIdentityData}));
    const auto options = service.messages().last().value("options").toObject();
    QCOMPARE(options.value("styleId").toString(), request.styleId);
    QCOMPARE(options.value("prompt").toString(), request.prompt);
    QCOMPARE(options.value("photoMakerModelId").toString(), request.photoMakerModelId);
    QCOMPARE(options.value("photoMakerStyleStrength").toInt(), 17);
    QVERIFY(!options.value("hasInput").toBool());
    request.photoMakerReferenceImageData = QByteArray::fromHex("89504e470d0a1a0a") + "reference";
    const auto reference = request.photoMakerReferenceImageData;
    const auto identity = request.photoMakerIdentityData;
    request.photoMakerIdentityPackageData = KiraService::packPhotoMakerIdentity(reference, identity);
    QVERIFY(client.submitGeneration(request).isEmpty()); // Ambiguous package + legacy inputs.
    request.photoMakerReferenceImageData.clear(); request.photoMakerIdentityData.clear();
    QVERIFY(!client.submitGeneration(request).isEmpty()); QTRY_COMPARE(results.count(), 2);
    QCOMPARE(service.uploadedImages().mid(2), QList<QByteArray>({reference, identity}));
    KiraService::PhotoMakerIdentityPackage full;
    full.references = {reference, reference + "2", reference + "3", reference + "4"};
    full.embeddings = identity;
    full.identityPrompt = "metadata only";
    full.hairPrompt = "metadata hair only";
    full.seed = 987;
    request.photoMakerIdentityPackageData = KiraService::packPhotoMakerIdentity(full);
    QVERIFY(!client.submitGeneration(request).isEmpty()); QTRY_COMPARE(results.count(), 3);
    auto uploads = full.references;
    uploads.append(identity);
    QCOMPARE(service.uploadedImages().mid(4), uploads);
    QCOMPARE(service.messages().last().value("options").toObject().value("prompt").toString(), request.prompt);
    QCOMPARE(service.messages().last().value("options").toObject().value("seed").toInt(), request.seed);
    request.photoMakerIdentityPackageData = "invalid";
    QVERIFY(client.submitGeneration(request).isEmpty());
    request.photoMakerIdentityPackageData.clear();
    request.photoMakerReferenceImageData = reference; request.photoMakerIdentityData = identity;
    QVERIFY(client.submitEdit(request).isEmpty());
    request.photoMakerStyleStrength = 101; QVERIFY(client.submitGeneration(request).isEmpty());
    request.photoMakerStyleStrength.reset(); request.photoMakerIdentityData.clear();
    QVERIFY(client.submitGeneration(request).isEmpty());
    client.disconnectFromService(); QTRY_VERIFY(!client.isAuthenticated());
    QVERIFY(!client.capabilities().photoMaker);
    FakeSdkService legacy; QVERIFY(legacy.start());
    client.setConfiguration(legacy.endpoint(), {}); client.connectToService(); QTRY_VERIFY(client.modelListAvailable());
    request.photoMakerIdentityData = "identity";
    QVERIFY(client.submitGeneration(request).isEmpty());
}

void ServiceSdkTest::submitsIpAdapterReferences()
{
    FakeSdkService service(true); QVERIFY(service.start());
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.capabilities().ipAdapter);
    QTRY_COMPARE(client.models(KiraService::ModelCategory::ClipVision).size(), 1);
    QCOMPARE(client.models(KiraService::ModelCategory::IpAdapter).size(), 1);
    KiraService::ImageGenerationRequest request;
    request.modelId = "image-generation:test.safetensors"; request.prompt = "test";
    request.styleId = "photographic";
    request.clipVisionModelId = "clip_vision:vision.safetensors";
    request.ipAdapterModelId = "ip-adapter:test.bin";
    request.ipAdapterImageData = "adapter reference"; request.ipAdapterStrength = 0.85;
    QSignalSpy results(&client, &KiraService::ServiceClient::imageJobFinished);
    QVERIFY(!client.submitGeneration(request).isEmpty()); QTRY_COMPARE(results.count(), 1);
    QCOMPARE(service.uploadedImages(), QList<QByteArray>{QByteArray("adapter reference")});
    auto options = service.messages().last().value("options").toObject();
    QCOMPARE(options.value("styleId").toString(), request.styleId);
    QCOMPARE(options.value("prompt").toString(), request.prompt);
    QCOMPARE(options.value("clipVisionModelId").toString(), request.clipVisionModelId);
    QCOMPARE(options.value("ipAdapterModelId").toString(), request.ipAdapterModelId);
    QCOMPARE(options.value("ip_adapter_strength").toDouble(), 0.85);
    QVERIFY(!options.value("hasInput").toBool());
    request.modelId = "edition:edit.safetensors"; request.baseImageData = "editing source";
    QVERIFY(client.submitEdit(request).isEmpty());
    request.styleId.clear();
    QVERIFY(!client.submitEdit(request).isEmpty()); QTRY_COMPARE(results.count(), 2);
    QCOMPARE(service.uploadedImages(), QList<QByteArray>({"adapter reference", "editing source", "adapter reference"}));
    request.ipAdapterImageData.clear(); QVERIFY(client.submitEdit(request).isEmpty());
    client.disconnectFromService(); QTRY_VERIFY(!client.isAuthenticated());
    QVERIFY(!client.capabilities().ipAdapter);
    FakeSdkService legacy; QVERIFY(legacy.start());
    client.setConfiguration(legacy.endpoint(), {}); client.connectToService(); QTRY_VERIFY(client.modelListAvailable());
    request.modelId = "image-generation:test.safetensors"; request.ipAdapterImageData = "reference";
    QVERIFY(!client.capabilities().ipAdapter); QVERIFY(client.submitGeneration(request).isEmpty());
}

void ServiceSdkTest::configuresAndCancelsModels()
{
    FakeSdkService service; QVERIFY(service.start()); KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService(); QTRY_VERIFY(client.modelListAvailable());
    QCOMPARE(client.models(KiraService::ModelCategory::Upscaler).first().recommendedOptions, QJsonObject({{"tileSize", 128}}));
    QSignalSpy configured(&client, &KiraService::ServiceClient::modelConfigured);
    QVERIFY(client.configureModel("upscaler:test.gguf", {{"tileSize", 256}}, true)); QTRY_COMPARE(configured.count(), 1); QCOMPARE(configured.first().at(1).value<QJsonObject>(), QJsonObject({{"tileSize", 256}}));
    const auto options = KiraService::Protocol::defaultModelOptions("llm-chat");
    const auto id = client.installModel(KiraService::ModelCategory::LlmChat, QUrl("https://example.test/model.gguf"), {}, options, {}, "checkpoint", {}, true); QVERIFY(!id.isEmpty());
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
    const auto editionModels = client.models(KiraService::ModelCategory::Edition);
    QCOMPARE(editionModels.size(), 1);
    QVERIFY(!editionModels.first().explicitContent);
    QVERIFY(client.models(KiraService::ModelCategory::ImageGeneration).first().explicitContent);
    QCOMPARE(editionModels.first().companionRole, QStringLiteral("vae"));
    QCOMPARE(client.models(KiraService::ModelCategory::LlmChat).first().companionRole, QStringLiteral("mmproj"));
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).first().modelType, QString("diffusion"));
    QVERIFY(client.models(KiraService::ModelCategory::ImageGeneration).first().styles.isEmpty());
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).first().companionFiles.value("clip-l").toString(), QString("companions/test.safetensors/clip-l/clip_l.safetensors"));
    QCOMPARE(client.models(KiraService::ModelCategory::ImageGeneration).first().companionFileName, QStringLiteral("companions/test.safetensors/vae.safetensors"));
    QCOMPARE(editionModels.first().id, QStringLiteral("edition:edit.safetensors"));
    QCOMPARE(editionModels.first().category, KiraService::ModelCategory::Edition);
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

void ServiceSdkTest::installsModelWithCompanion()
{
    FakeSdkService service; QVERIFY(service.start());
    KiraService::ServiceClient client; client.setConfiguration(service.endpoint(), {}); client.connectToService();
    QTRY_VERIFY(client.modelListAvailable());
    const QUrl companion(QStringLiteral("https://example.test/mmproj.gguf"));
    QVERIFY(!client.installModel(KiraService::ModelCategory::LlmChat, QUrl(QStringLiteral("https://example.test/model.gguf")), companion).isEmpty());
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
