#include "KiraService.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include <kira/service/ServiceProtocol.h>
#include <kira/service/ModelOptions.h>

#include <QDir>
#include <QScopeGuard>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QtTest>

namespace
{
class TestHttpServer final : public QObject
{
public:
    explicit TestHttpServer(QByteArray payload)
        : m_payload(std::move(payload))
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                const QByteArray request = socket->readAll();
                if (request.contains("slow.gguf")) return;
                if (request.contains("missing.safetensors")) {
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost(); return;
                }
                const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(m_payload.size()) + "\r\nConnection: close\r\n\r\n" + m_payload;
                socket->write(response); socket->disconnectFromHost();
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }
    QUrl url(const QString &fileName = QStringLiteral("model.gguf")) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/owner/repo/resolve/main/%2?utm_source=chatgpt.com").arg(m_server.serverPort()).arg(fileName));
    }

private:
    QTcpServer m_server;
    QByteArray m_payload;
};
}

class ServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void generatesWithValidatedOptionsAndVae();
    void validatesRecommendedOptions();
    void persistsOptionsAndLegacyMetadata();
    void reservesSingleInstallAndCancels();
    void cancelsDuringCompanionDownload();
    void rollsBackMetadataFailure();
    void configuresModelsOverProtocol();
    void preservesProtocolV3();
    void rejectsLanWithoutToken();
    void startsOnLoopbackAndLanAddress();
    void authenticatesLoopbackWithoutToken();
    void rejectsLanAuthenticationWithoutToken();
    void rollsBackWhenLanListenerFails();
    void listsSupportedModelsWithoutPathLeak();
    void removesModelByOpaqueId();
    void installsModelAtomically();
    void installsIpAdapterPair();
    void managesEditionModels();
    void installsOptionalCompanions();
    void attachesModularCompanionsWithoutReplacingModel();
    void attachesFilesOverProtocol();
    void rollsBackFailedCompanion();
    void acceptsExternalModelUrls();
    void rejectsInvalidModelInstall();
    void migratesLegacyModelRoot();
    void rejectsConflictingModelRoots();
};

namespace
{
QHostAddress availableLanAddress()
{
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback() && !entry.ip().isNull()) return entry.ip();
        }
    }
    return {};
}
}

void ServiceTest::generatesWithValidatedOptionsAndVae()
{
#ifdef Q_OS_WIN
    QSKIP("The fake sd-cli fixture requires a POSIX shell.");
#endif
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray oldPath = qgetenv("PATH");
    const auto restore = qScopeGuard(
        [oldPath]
        {
            qputenv("PATH", oldPath);
            qunsetenv("KIRA_TEST_ARGS");
            qunsetenv("KIRA_TEST_IMAGE");
        });
    qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
    const QString capture = directory.filePath("args.txt");
    qputenv("KIRA_TEST_ARGS", capture.toUtf8());
    const QByteArray image = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1kAAAAASUVORK5CYII=");
    QFile imageFile(directory.filePath("fixture.png"));
    QVERIFY(imageFile.open(QIODevice::WriteOnly));
    imageFile.write(image);
    imageFile.close();
    qputenv("KIRA_TEST_IMAGE", imageFile.fileName().toUtf8());
    QFile executable(directory.filePath("sd-cli"));
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$KIRA_TEST_ARGS\"\nprevious=\"\"\nfor argument in "
                     "\"$@\"; do\n if [ \"$previous\" = --output ]; then cp \"$KIRA_TEST_IMAGE\" "
                     "\"$argument\"; fi\n previous=\"$argument\"\ndone\nexit 0\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString root = directory.filePath("models");
    const QString companion = "companions/model.gguf/vae with space.safetensors";
    const QString clipL = "companions/model.gguf/clip_l.safetensors";
    const QString clipG = "companions/model.gguf/clip_g.safetensors";
    QVERIFY(QDir().mkpath(QDir(root).filePath("companions/model.gguf")));
    for (const QString &name : {QString("model.gguf"), QString("plain.gguf"), companion, clipL, clipG})
    {
        QFile file(QDir(root).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture");
    }
    QFile metadata(QDir(root).filePath("metadata.json"));
    QVERIFY(metadata.open(QIODevice::WriteOnly));
    metadata.write(QJsonDocument(QJsonObject{{"model.gguf",
                                              QJsonObject{{"companionFileName", companion},
                                                          {"recommendedOptions",
                                                           QJsonObject{{"steps", 99}, {"cfgScale", 9.5}}}}}})
                       .toJson());
    metadata.close();
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = root;
    KiraServiceDaemon service(configuration);
    QString error;
    QVERIFY2(service.start(&error), qPrintable(error));
    QWebSocket socket;
    QList<QJsonObject> messages;
    QSignalSpy images(&socket, &QWebSocket::binaryMessageReceived);
    connect(&socket, &QWebSocket::textMessageReceived, this,
            [&](const QString &text) { messages.append(QJsonDocument::fromJson(text.toUtf8()).object()); });
    socket.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())));
    QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
    socket.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}");
    QTRY_VERIFY(messages.size() >= 2);
    messages.clear();
    const auto send = [&](const QString &id, const QJsonObject &options)
    {
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "submit"},
                                                                           {"operation", "image.generate"},
                                                                           {"jobId", id},
                                                                           {"options", options}})
                                                     .toJson(QJsonDocument::Compact)));
    };
    const auto terminal = [&](const QString &id)
    {
        for (const auto &message : messages)
            if (message.value("jobId").toString() == id &&
                (message.value("type") == "error" || message.value("type") == "result"))
                return true;
        return false;
    };
    const QJsonObject valid{{"modelId", "image-generation:model.gguf"},
                            {"prompt", "test"},
                            {"negativePrompt", ""},
                            {"width", 896},
                            {"height", 1152},
                            {"steps", 37},
                            {"cfgScale", 5.75},
                            {"seed", -1},
                            {"sampler", ""},
                            {"scheduler", ""},
                            {"clipSkip", -1},
                            {"hasInput", false},
                            {"baseImageStrength", 0.55}};
    const QList<QPair<QString, QJsonValue>> invalid = {{"prompt", 42},
                                                       {"prompt", " "},
                                                       {"negativePrompt", false},
                                                       {"width", 64.5},
                                                       {"width", "1024"},
                                                       {"height", 0},
                                                       {"height", 4097},
                                                       {"steps", 0},
                                                       {"steps", 201},
                                                       {"steps", 28.5},
                                                       {"cfgScale", -0.1},
                                                       {"cfgScale", 100.1},
                                                       {"cfgScale", "7"},
                                                       {"cfgScale", QJsonValue(QJsonValue::Null)},
                                                       {"seed", -2},
                                                       {"seed", 2147483648.0},
                                                       {"seed", 1.5},
                                                       {"seed", true},
                                                       {"clipSkip", 0},
                                                       {"clipSkip", 13},
                                                       {"clipSkip", -2},
                                                       {"clipSkip", 1.5},
                                                       {"clipSkip", "2"},
                                                       {"sampler", "unknown"},
                                                       {"sampler", true},
                                                       {"scheduler", "unknown"},
                                                       {"scheduler", QJsonValue(QJsonValue::Null)},
                                                       {"hasInput", "true"},
                                                       {"baseImageStrength", -0.1},
                                                       {"baseImageStrength", 1.1},
                                                       {"baseImageStrength", "0.5"}};
    int index = 0;
    for (const auto &field : invalid)
    {
        QJsonObject options = valid;
        options.insert(field.first, field.second);
        const QString id = QString("invalid-%1").arg(index++);
        messages.clear();
        send(id, options);
        QTRY_VERIFY(terminal(id));
        QCOMPARE(messages.last().value("code").toString(), QString("invalid-request"));
        QVERIFY(!QFileInfo::exists(capture));
    }
    const auto arguments = [&]
    {
        QFile file(capture);
        if (!file.open(QIODevice::ReadOnly))
            return QStringList{};
        return QString::fromUtf8(file.readAll()).split('\n');
    };
    messages.clear();
    send("automatic", valid);
    QTRY_VERIFY(terminal("automatic"));
    QTRY_COMPARE(images.count(), 1);
    QCOMPARE(images.last().first().toByteArray(), image);
    auto args = arguments();
    QVERIFY(!args.contains("--sampling-method"));
    QVERIFY(!args.contains("--scheduler"));
    QVERIFY(!args.contains("--clip-skip"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("5.75"));
    QCOMPARE(args.value(args.indexOf("--steps") + 1), QString("37"));
    QCOMPARE(args.value(args.indexOf("--vae") + 1), QDir(root).filePath(companion));
    QVERIFY(args.contains("--model"));
    QVERIFY(!args.contains("--diffusion-model"));
    // Associate modular files without changing the caller's generation options.
    QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    metadata.write(QJsonDocument(QJsonObject{{"model.gguf", QJsonObject{
        {"modelType", "diffusion"}, {"companionFiles", QJsonObject{
            {"vae", companion}, {"clip-l", clipL}, {"clip-g", clipG}}}}}}).toJson());
    metadata.close();
    messages.clear();
    send("modular", valid);
    QTRY_VERIFY(terminal("modular"));
    QTRY_COMPARE(images.count(), 2);
    args = arguments();
    QVERIFY(!args.contains("--model"));
    QCOMPARE(args.value(args.indexOf("--diffusion-model") + 1), QDir(root).filePath("model.gguf"));
    QCOMPARE(args.value(args.indexOf("--clip_l") + 1), QDir(root).filePath(clipL));
    QCOMPARE(args.value(args.indexOf("--clip_g") + 1), QDir(root).filePath(clipG));
    QCOMPARE(args.value(args.indexOf("--vae") + 1), QDir(root).filePath(companion));
    QJsonObject explicitOptions = valid;
    explicitOptions.insert("sampler", "dpm++2m");
    explicitOptions.insert("scheduler", "karras");
    explicitOptions.insert("clipSkip", 12);
    explicitOptions.insert("cfgScale", 7.125);
    explicitOptions.insert("seed", 2147483647);
    explicitOptions.insert("hasInput", true);
    explicitOptions.insert("baseImageStrength", 0.5678);
    messages.clear();
    send("explicit", explicitOptions);
    QTRY_VERIFY(!messages.isEmpty() && messages.last().value("type") == "upload-required");
    socket.sendBinaryMessage(QByteArray("fixture input"));
    QTRY_VERIFY(terminal("explicit"));
    QTRY_COMPARE(images.count(), 3);
    args = arguments();
    QCOMPARE(args.value(args.indexOf("--sampling-method") + 1), QString("dpm++2m"));
    QCOMPARE(args.value(args.indexOf("--scheduler") + 1), QString("karras"));
    QCOMPARE(args.value(args.indexOf("--clip-skip") + 1), QString("12"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("7.125"));
    QCOMPARE(args.value(args.indexOf("--strength") + 1).toDouble(), 0.5678);
    QCOMPARE(args.value(args.indexOf("--vae") + 1), QDir(root).filePath(companion));
    QVERIFY(args.contains("--init-img"));
    QJsonObject minimal{{"modelId", "image-generation:plain.gguf"},
                        {"prompt", "test"},
                        {"width", 64},
                        {"height", 4096},
                        {"steps", 200}};
    messages.clear();
    send("minimal", minimal);
    QTRY_VERIFY(terminal("minimal"));
    QTRY_COMPARE(images.count(), 4);
    args = arguments();
    QVERIFY(!args.contains("--vae"));
    QVERIFY(!args.contains("--sampling-method"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("7"));
    QCOMPARE(args.value(args.indexOf("--seed") + 1), QString("-1"));
    for (const auto &message : messages)
        QVERIFY(!QJsonDocument(message).toJson().contains(root.toUtf8()));
    int completed = 4;
    for (double cfg : {0.0, 100.0})
    {
        auto boundary = valid;
        boundary.insert("cfgScale", cfg);
        boundary.insert("clipSkip", 1);
        boundary.insert("seed", 0);
        boundary.insert("baseImageStrength", cfg == 0 ? 0.0 : 1.0);
        boundary.insert("sampler", "euler");
        boundary.insert("scheduler", "discrete");
        const QString id = QString("boundary-%1").arg(cfg);
        messages.clear();
        send(id, boundary);
        QTRY_VERIFY(terminal(id));
        ++completed;
        QTRY_COMPARE(images.count(), completed);
        args = arguments();
        QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1).toDouble(), cfg);
    }
}

void ServiceTest::validatesRecommendedOptions()
{
    using namespace KiraService::Protocol;
    for (auto category : {"image-generation", "edition", "llm-chat", "upscaler", "ip-adapter"}) {
        auto options = defaultModelOptions(category); QVERIFY(validateModelOptions(category, options));
        options.insert("unrelated", true); QVERIFY(!validateModelOptions(category, options));
    }
    auto image = defaultModelOptions("image-generation"); image.insert("steps", "28"); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("steps", 28.5); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("steps", 200); image.insert("width", 2048); image.insert("height", 2048); QVERIFY(validateModelOptions("image-generation", image));
    image.insert("clipSkip", 0); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("clipSkip", 12); image.insert("sampler", "unknown"); QVERIFY(!validateModelOptions("image-generation", image));
    auto llm = defaultModelOptions("llm-chat"); llm.insert("maxTokens", 4097); QVERIFY(!validateModelOptions("llm-chat", llm));
    llm.insert("maxTokens", 4096); llm.insert("topP", 0); QVERIFY(!validateModelOptions("llm-chat", llm));
    QVERIFY(!validateModelOptions("upscaler", {{"tileSize", 129}}));
    QVERIFY(!validateModelOptions("ip-adapter", {{"strength", 2.1}}));
}

void ServiceTest::persistsOptionsAndLegacyMetadata()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(QDir().mkpath(directory.filePath("companions/legacy.gguf")));
    for (const QString &name : {QString("legacy.gguf"), QString("companions/legacy.gguf/vae.gguf"), QString("untouched.gguf")}) {
        QFile file(directory.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("model");
    }
    QFile file(directory.filePath("metadata.json")); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"legacy.gguf\":\"companions/legacy.gguf/vae.gguf\",\"untouched.gguf\":\"companions/untouched.gguf/vae.gguf\"}"); file.close();
    ModelCatalog catalog(directory.path(), true); const QString id = "image-generation:legacy.gguf";
    QVERIFY(catalog.modelById(id)); QVERIFY(catalog.modelById(id)->recommendedOptions.isEmpty());
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation"); QString error;
    QVERIFY2(catalog.configure(id, options, &error), qPrintable(error));
    ModelCatalog restored(directory.path(), true); QCOMPARE(restored.modelById(id)->recommendedOptions, options);
    QCOMPARE(restored.modelById(id)->companionFileName, QString("companions/legacy.gguf/vae.gguf"));
    QVERIFY(file.open(QIODevice::ReadOnly)); auto metadata = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    QVERIFY(metadata.value("untouched.gguf").isString());
    auto bad = options; bad.insert("steps", -1); QVERIFY(!catalog.configure(id, bad, &error)); QCOMPARE(catalog.modelById(id)->recommendedOptions, options);
    QVERIFY(restored.remove(id, &error)); QVERIFY(file.open(QIODevice::ReadOnly)); metadata = QJsonDocument::fromJson(file.readAll()).object(); file.close(); QVERIFY(!metadata.contains("legacy.gguf"));
    QFile legacy(directory.filePath("legacy.gguf")); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write("model"); legacy.close();
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("broken json"); file.close();
    QVERIFY(!catalog.configure(id, options, &error)); QVERIFY(!catalog.remove(id, &error));
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("broken json")); QVERIFY(QFileInfo::exists(legacy.fileName()));
}

void ServiceTest::reservesSingleInstallAndCancels()
{
    TestHttpServer server("model"); QVERIFY(server.start()); QTemporaryDir directory; ModelCatalog catalog(directory.path(), true);
    QSignalSpy canceled(&catalog, &ModelCatalog::installCanceled); QSignalSpy completed(&catalog, &ModelCatalog::installFinished); QString error;
    QVERIFY(catalog.install({"first", ModelCategory::Upscaler, server.url(), {}}, &error)); QVERIFY(catalog.isInstalling());
    QVERIFY(!catalog.install({"second", ModelCategory::Upscaler, server.url("other.gguf"), {}}, &error));
    catalog.cancel("first"); QCOMPARE(canceled.count(), 1); QVERIFY(!catalog.isInstalling());
    QVERIFY(catalog.install({"third", ModelCategory::Upscaler, server.url(), {}, KiraService::Protocol::defaultModelOptions("upscaler")}, &error));
    QTRY_COMPARE(completed.count(), 1); QCOMPARE(catalog.models(ModelCategory::Upscaler).first().recommendedOptions, KiraService::Protocol::defaultModelOptions("upscaler"));
    QVERIFY(!QFileInfo::exists(directory.filePath("upscale/model.gguf.part")));
}

void ServiceTest::cancelsDuringCompanionDownload()
{
    TestHttpServer server("model"); QVERIFY(server.start()); QTemporaryDir directory; ModelCatalog catalog(directory.path(), true);
    QSignalSpy canceled(&catalog, &ModelCatalog::installCanceled); QSignalSpy completed(&catalog, &ModelCatalog::installFinished); QSignalSpy progress(&catalog, &ModelCatalog::installProgress); QString error;
    QVERIFY(catalog.install({"pair", ModelCategory::Edition, server.url(), server.url("slow.gguf"), KiraService::Protocol::defaultModelOptions("edition")}, &error));
    const auto companionStarted = [&progress] {
        for (const auto &event : progress) if (event.first().value<ModelInstallProgress>().fileRole == "companion") return true;
        return false;
    };
    QTRY_VERIFY(companionStarted());
    catalog.cancel("pair"); QCOMPARE(canceled.count(), 1); QCOMPARE(completed.count(), 0); QVERIFY(!catalog.isInstalling());
    QVERIFY(!QFileInfo::exists(directory.filePath("edition/model.gguf.part"))); QVERIFY(!QFileInfo::exists(directory.filePath("edition/companions/model.gguf/slow.gguf.part"))); QVERIFY(catalog.models(ModelCategory::Edition).isEmpty());
}

void ServiceTest::rollsBackMetadataFailure()
{
    TestHttpServer server("model"); QVERIFY(server.start()); QTemporaryDir directory; ModelCatalog catalog(directory.path(), true);
    QSignalSpy failed(&catalog, &ModelCatalog::installFailed); QSignalSpy completed(&catalog, &ModelCatalog::installFinished); QString error;
    QVERIFY(catalog.install({"broken", ModelCategory::Edition, server.url(), server.url("vae.gguf"), KiraService::Protocol::defaultModelOptions("edition")}, &error));
    QVERIFY(QDir().mkpath(directory.filePath("edition/metadata.json")));
    QTRY_COMPARE(failed.count(), 1); QCOMPARE(completed.count(), 0); QVERIFY(catalog.models(ModelCategory::Edition).isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath("edition/model.gguf"))); QVERIFY(!QFileInfo::exists(directory.filePath("edition/companions/model.gguf/vae.gguf")));
}

void ServiceTest::configuresModelsOverProtocol()
{
    QTemporaryDir directory; QFile model(directory.filePath("test.gguf")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
    KiraServiceConfiguration configuration; configuration.port = 0; configuration.modelRootPath = directory.path(); KiraServiceDaemon service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
    QWebSocket client; QSignalSpy received(&client, &QWebSocket::textMessageReceived); client.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort()))); QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
    const auto send = [&client](const QJsonObject &object) { client.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))); };
    send({{"type", "authenticate"}, {"version", 3}}); QTRY_VERIFY(received.count() >= 2); received.clear();
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    send({{"type", "models.configure"}, {"id", "image-generation:test.gguf"}, {"recommendedOptions", options}}); QTRY_COMPARE(received.count(), 1);
    auto response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object(); QCOMPARE(response.value("type").toString(), QString("models.configure.completed")); QCOMPARE(response.value("model").toObject().value("recommendedOptions").toObject(), options); QVERIFY(!response.value("model").toObject().contains("filePath"));
    send({{"type", "models.list"}}); QTRY_COMPARE(received.count(), 1); response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object();
    QCOMPARE(response.value("categories").toArray().first().toObject().value("models").toArray().first().toObject().value("recommendedOptions").toObject(), options);
    send({{"type", "models.configure"}, {"id", "image-generation:test.gguf"}, {"recommendedOptions", true}}); QTRY_COMPARE(received.count(), 1); response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object(); QCOMPARE(response.value("type").toString(), QString("error"));
    QTcpServer stalledDownload; QVERIFY(stalledDownload.listen(QHostAddress::LocalHost));
    const QString url = QString("https://127.0.0.1:%1/one.gguf").arg(stalledDownload.serverPort());
    QWebSocket stranger; QSignalSpy strangerMessages(&stranger, &QWebSocket::textMessageReceived);
    stranger.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort()))); QTRY_COMPARE(stranger.state(), QAbstractSocket::ConnectedState);
    stranger.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}"); QTRY_VERIFY(strangerMessages.count() >= 2); strangerMessages.clear();
    send({{"type", "models.install"}, {"installId", "one"}, {"category", "upscaler"}, {"url", url}});
    const auto contains = [](const QSignalSpy &spy, const QString &key, const QString &value) {
        for (const auto &message : spy) if (QJsonDocument::fromJson(message.first().toString().toUtf8()).object().value(key).toString() == value) return true;
        return false;
    };
    QTRY_VERIFY(contains(received, "type", "models.install.accepted"));
    stranger.sendTextMessage("{\"type\":\"models.install.cancel\",\"installId\":\"one\"}"); QTRY_VERIFY(contains(strangerMessages, "code", "invalid-model-cancel"));
    send({{"type", "models.install"}, {"installId", "two"}, {"category", "upscaler"}, {"url", url}}); QTRY_VERIFY(contains(received, "code", "model-install-busy"));
    send({{"type", "models.install.cancel"}, {"installId", "one"}}); QTRY_VERIFY(contains(received, "type", "models.install.canceled"));
    send({{"type", "models.install"}, {"installId", "three"}, {"category", "upscaler"}, {"url", url}});
    QTRY_VERIFY(contains(received, "installId", "three")); client.close(); QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
    stranger.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.install"}, {"installId", "four"}, {"category", "upscaler"}, {"url", url}}).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY(contains(strangerMessages, "type", "models.install.accepted")); stranger.close();

}

void ServiceTest::preservesProtocolV3()
{
    QCOMPARE(KiraService::Protocol::Version, 3);
    QCOMPARE(KiraService::Protocol::DefaultPort, quint16(9384));
    QVERIFY(ModelCatalog::categories().contains(ModelCategory::IpAdapter));
    QVERIFY(ModelCatalog::categoryFromName(QStringLiteral("ip-adapter")).has_value());
    QCOMPARE(*ModelCatalog::categoryFromName(QStringLiteral("ip-adapter")), ModelCategory::IpAdapter);
}

void ServiceTest::rejectsLanWithoutToken()
{
    const QHostAddress lan = availableLanAddress();
    if (lan.isNull()) QSKIP("No non-loopback IPv4 address is available");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = lan; configuration.port = 0; configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration); QString error;
    QVERIFY(!service.start(&error)); QVERIFY(error.contains(QStringLiteral("token"), Qt::CaseInsensitive));
}

void ServiceTest::startsOnLoopbackAndLanAddress()
{
    const QHostAddress lan = availableLanAddress();
    if (lan.isNull()) QSKIP("No non-loopback IPv4 address is available");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = lan; configuration.port = 0; configuration.token = QStringLiteral("lan-secret"); configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
    QTcpSocket local; local.connectToHost(QHostAddress(QStringLiteral("127.0.0.1")), service.serverPort()); QVERIFY(local.waitForConnected(1000));
    QTcpSocket network; network.connectToHost(lan, service.serverPort()); QVERIFY(network.waitForConnected(1000));
}

void ServiceTest::authenticatesLoopbackWithoutToken()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = QHostAddress(QStringLiteral("127.0.0.1")); configuration.port = 0; configuration.token = QStringLiteral("lan-secret"); configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
    QWebSocket client; QSignalSpy messages(&client, &QWebSocket::textMessageReceived);
    client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(service.serverPort()))); QTRY_VERIFY_WITH_TIMEOUT(client.state() == QAbstractSocket::ConnectedState, 1000);
    client.sendTextMessage(QStringLiteral("{\"type\":\"authenticate\",\"version\":3,\"token\":\"\"}"));
    QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 1000);
    bool authenticated = false;
    for (const QList<QVariant> &message : messages) if (QJsonDocument::fromJson(message.first().toString().toUtf8()).object().value(QStringLiteral("type")).toString() == QStringLiteral("authenticated")) authenticated = true;
    QVERIFY(authenticated);
}

void ServiceTest::rejectsLanAuthenticationWithoutToken()
{
    const QHostAddress lan = availableLanAddress();
    if (lan.isNull()) QSKIP("No non-loopback IPv4 address is available");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = lan; configuration.port = 0; configuration.token = QStringLiteral("lan-secret"); configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
    QWebSocket client; QSignalSpy messages(&client, &QWebSocket::textMessageReceived);
    client.open(QUrl(QStringLiteral("ws://%1:%2").arg(lan.toString()).arg(service.serverPort()))); QTRY_VERIFY_WITH_TIMEOUT(client.state() == QAbstractSocket::ConnectedState, 1000);
    client.sendTextMessage(QStringLiteral("{\"type\":\"authenticate\",\"version\":3,\"token\":\"\"}"));
    QTRY_VERIFY_WITH_TIMEOUT(messages.count() >= 1, 1000);
    bool rejected = false;
    for (const QList<QVariant> &message : messages) {
        const QJsonObject response = QJsonDocument::fromJson(message.first().toString().toUtf8()).object();
        if (response.value(QStringLiteral("type")).toString() == QStringLiteral("error") && response.value(QStringLiteral("code")).toString() == QStringLiteral("authentication-failed")) rejected = true;
    }
    QVERIFY(rejected);
}

void ServiceTest::rollsBackWhenLanListenerFails()
{
    const QHostAddress lan = availableLanAddress();
    if (lan.isNull()) QSKIP("No non-loopback IPv4 address is available");
    QTcpServer blocker; QVERIFY(blocker.listen(lan, 0));
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = lan; configuration.port = blocker.serverPort(); configuration.token = QStringLiteral("lan-secret"); configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration); QString error; QVERIFY(!service.start(&error));
    QTcpSocket local; local.connectToHost(QHostAddress(QStringLiteral("127.0.0.1")), blocker.serverPort()); QVERIFY(!local.waitForConnected(100));
}

void ServiceTest::installsModelAtomically()
{
    TestHttpServer server(QByteArray("model-bytes")); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished); QString error;
    QVERIFY(catalog.install({QStringLiteral("install-1"), ModelCategory::Upscaler, server.url(), {}}, &error));
    QTRY_COMPARE(finished.count(), 1);
    const QString finalPath = directory.filePath(QStringLiteral("upscale/model.gguf"));
    QVERIFY(QFileInfo::exists(finalPath)); QVERIFY(!QFileInfo::exists(finalPath + QStringLiteral(".part")));
}

void ServiceTest::managesEditionModels()
{
    QCOMPARE(ModelCatalog::categoryFromName(QStringLiteral("edition")), std::optional<ModelCategory>(ModelCategory::Edition));
    QTemporaryDir directory; QVERIFY(directory.isValid());
    ModelCatalog catalog(directory.path(), true);
    TestHttpServer server(QByteArray("edition-model")); QVERIFY(server.start());
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    for (const QString &extension : {QStringLiteral("safetensors"), QStringLiteral("ckpt"), QStringLiteral("gguf")}) {
        const QString fileName = QStringLiteral("edit-model.%1").arg(extension);
        QString error;
        const int previousCount = finished.count();
        QVERIFY(catalog.install({fileName, ModelCategory::Edition, server.url(fileName), {}}, &error));
        QTRY_COMPARE(finished.count(), previousCount + 1);
        const QString path = directory.filePath(QStringLiteral("edition/") + fileName);
        QVERIFY(QFileInfo::exists(path));
        const QString id = QStringLiteral("edition:") + fileName;
        const auto model = catalog.modelById(id); QVERIFY(model.has_value());
        QCOMPARE(model->filePath, path);
        QCOMPARE(catalog.models(ModelCategory::Edition).size(), 1);
        QVERIFY(catalog.models(ModelCategory::ImageGeneration).isEmpty());
        ModelCatalog restored(directory.path(), true);
        QVERIFY(restored.modelById(id).has_value());
        QVERIFY(catalog.remove(id, &error));
        QVERIFY(!QFileInfo::exists(path));
    }
}

void ServiceTest::installsIpAdapterPair()
{
    TestHttpServer server(QByteArray("model-bytes")); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished); QString error;
    const ModelInstallRequest request{QStringLiteral("ip-adapter-install"), ModelCategory::IpAdapter,
                                      server.url(QStringLiteral("ip-adapter-faceid-plusv2_sdxl.bin")),
                                      server.url(QStringLiteral("ip-adapter-faceid-plusv2_sdxl_lora.safetensors"))};
    QVERIFY(catalog.install(request, &error));
    QTRY_COMPARE(finished.count(), 1);
    const QString primaryPath = directory.filePath(QStringLiteral("ip-adapter/ip-adapter-faceid-plusv2_sdxl.bin"));
    const QString companionRelativePath = QStringLiteral("companions/ip-adapter-faceid-plusv2_sdxl.bin/ip-adapter-faceid-plusv2_sdxl_lora.safetensors");
    const QString companionPath = directory.filePath(QStringLiteral("ip-adapter/%1").arg(companionRelativePath));
    QVERIFY(QFileInfo::exists(primaryPath));
    QVERIFY(QFileInfo::exists(companionPath));
    QVERIFY(!QFileInfo::exists(primaryPath + QStringLiteral(".part")));
    const QList<ModelInfo> models = catalog.models(ModelCategory::IpAdapter);
    QCOMPARE(models.size(), 1);
    QCOMPARE(models.first().companionFileName, companionRelativePath);
    ModelCatalog restoredCatalog(directory.path(), true);
    QCOMPARE(restoredCatalog.models(ModelCategory::IpAdapter).first().companionFileName, companionRelativePath);
    QVERIFY(catalog.remove(models.first().id, &error));
    QVERIFY(!QFileInfo::exists(primaryPath));
    QVERIFY(!QFileInfo::exists(companionPath));
}

void ServiceTest::attachesModularCompanionsWithoutReplacingModel()
{
    TestHttpServer server("companion bytes");
    QVERIFY(server.start());
    QTemporaryDir directory;
    QFile primary(directory.filePath("existing.gguf"));
    QVERIFY(primary.open(QIODevice::WriteOnly));
    primary.write("original model");
    primary.close();
    ModelCatalog catalog(directory.path(), true);
    ModelInstallRequest request;
    request.installId = "attach";
    request.existingModelId = "image-generation:existing.gguf";
    request.modelType = "diffusion";
    request.companionUrls = {{"clip-l", server.url("clip_l.safetensors").toString()},
        {"clip-g", server.url("clip_g.safetensors").toString()},
        {"vae", server.url("vae.safetensors").toString()}};
    QString error;
    QSignalSpy done(&catalog, &ModelCatalog::installFinished);
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(done.count(), 1);
    ModelCatalog restored(directory.path(), true);
    const auto model = restored.modelById(request.existingModelId);
    QVERIFY(model);
    QCOMPARE(model->modelType, QString("diffusion"));
    QCOMPARE(model->companionFiles.size(), 3);
    QVERIFY(primary.open(QIODevice::ReadOnly));
    QCOMPARE(primary.readAll(), QByteArray("original model"));
    primary.close();
    request.installId = "duplicate";
    QVERIFY(!restored.install(request, &error));
    request.companionUrls = {{"unknown", server.url().toString()}};
    QVERIFY(!restored.install(request, &error));
    request.companionUrls = {{"clip-l", true}};
    QVERIFY(!restored.install(request, &error));
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    QVERIFY(restored.configure(request.existingModelId, options, &error));
    QCOMPARE(restored.modelById(request.existingModelId)->companionFiles, model->companionFiles);
    QVERIFY(restored.remove(request.existingModelId, &error));
    for (const auto &name : model->companionFiles)
        QVERIFY(!QFileInfo::exists(directory.filePath(name.toString())));

    // Failure during a later companion leaves the existing primary untouched.
    QVERIFY(primary.open(QIODevice::WriteOnly));
    primary.write("original model");
    primary.close();
    request.installId = "failed-attach";
    request.companionUrls = {{"clip-l", server.url("clip_l.safetensors").toString()},
        {"clip-g", server.url("missing.safetensors").toString()}};
    QSignalSpy failed(&restored, &ModelCatalog::installFailed);
    QVERIFY(restored.install(request, &error));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(primary.exists());
    QVERIFY(restored.modelById(request.existingModelId)->companionFiles.isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath("companions/existing.gguf/clip-l/clip_l.safetensors.part")));
    QVERIFY(!QFileInfo::exists(directory.filePath("companions/existing.gguf/clip-l/clip_l.safetensors")));
    request.installId = "cancel-attach";
    request.companionUrls = {{"clip-g", server.url("clip_g.safetensors").toString()},
        {"clip-l", server.url("slow.gguf").toString()}};
    QSignalSpy progress(&restored, &ModelCatalog::installProgress);
    QSignalSpy canceled(&restored, &ModelCatalog::installCanceled);
    QVERIFY(restored.install(request, &error));
    const auto secondFileStarted = [&] {
        for (const auto &event : progress)
            if (event.first().value<ModelInstallProgress>().fileRole == "clip-l") return true;
        return false;
    };
    QTRY_VERIFY(secondFileStarted());
    restored.cancel(request.installId);
    QCOMPARE(canceled.count(), 1);
    QVERIFY(primary.exists());
    QVERIFY(restored.modelById(request.existingModelId)->companionFiles.isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath("companions/existing.gguf/clip-g/clip_g.safetensors.part")));
    QVERIFY(!QFileInfo::exists(directory.filePath("companions/existing.gguf/clip-l/slow.gguf.part")));
}

void ServiceTest::attachesFilesOverProtocol()
{
    QTemporaryDir directory;
    QFile primary(directory.filePath("existing.gguf"));
    QVERIFY(primary.open(QIODevice::WriteOnly));
    primary.write("original model");
    primary.close();
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration);
    QString error;
    QVERIFY(service.start(&error));
    QWebSocket socket;
    QList<QJsonObject> messages;
    connect(&socket, &QWebSocket::textMessageReceived, this, [&](const QString &text) {
        messages.append(QJsonDocument::fromJson(text.toUtf8()).object());
    });
    socket.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())));
    QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
    socket.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}");
    QTRY_VERIFY(messages.size() >= 2);
    messages.clear();
    socket.sendTextMessage("{\"type\":\"models.install\",\"installId\":\"attach\",\"category\":\"image-generation\",\"id\":\"image-generation:existing.gguf\",\"modelType\":\"diffusion\",\"companionUrls\":{}}");
    QTRY_VERIFY(messages.size() >= 2);
    QCOMPARE(messages.first().value("type").toString(), QString("models.install.accepted"));
    QCOMPARE(messages.last().value("type").toString(), QString("models.install.completed"));
    QCOMPARE(messages.last().value("model").toObject().value("modelType").toString(), QString("diffusion"));
    for (const auto &message : messages)
        QVERIFY(!QJsonDocument(message).toJson().contains(directory.path().toUtf8()));
    ModelCatalog catalog(directory.path(), true);
    QCOMPARE(catalog.modelById("image-generation:existing.gguf")->modelType, QString("diffusion"));
    messages.clear();
    socket.sendTextMessage("{\"type\":\"models.install\",\"installId\":\"preserve-type\",\"category\":\"image-generation\",\"id\":\"image-generation:existing.gguf\"}");
    QTRY_VERIFY(messages.size() >= 2);
    QCOMPARE(messages.last().value("model").toObject().value("modelType").toString(), QString("diffusion"));
    messages.clear();
    socket.sendTextMessage("{\"type\":\"models.install\",\"installId\":\"bad\",\"category\":\"image-generation\",\"id\":\"image-generation:existing.gguf\",\"companionUrls\":true}");
    QTRY_VERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().value("code").toString(), QString("invalid-model-install"));
}

void ServiceTest::installsOptionalCompanions()
{
    TestHttpServer server(QByteArray("model-bytes")); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    for (const ModelCategory category : {ModelCategory::LlmChat, ModelCategory::ImageGeneration, ModelCategory::Edition}) {
        const QString categoryName = ModelCatalog::categoryName(category);
        const QString primaryName = QStringLiteral("shared.gguf");
        const QString companionName = category == ModelCategory::LlmChat ? QStringLiteral("mmproj.gguf") : QStringLiteral("vae.safetensors");
        QString error;
        const int count = finished.count();
        QVERIFY(catalog.install({categoryName, category, server.url(primaryName), server.url(companionName)}, &error));
        QTRY_COMPARE(finished.count(), count + 1);
        const QString id = categoryName + QLatin1Char(':') + primaryName;
        const auto model = catalog.modelById(id); QVERIFY(model.has_value());
        QCOMPARE(model->companionRole, category == ModelCategory::LlmChat ? QStringLiteral("mmproj") : QStringLiteral("vae"));
        QCOMPARE(model->companionFileName, QStringLiteral("companions/shared.gguf/") + companionName);
        const QString companionPath = QDir(QFileInfo(model->filePath).absolutePath()).filePath(model->companionFileName);
        QVERIFY(QFileInfo::exists(companionPath));
        QVERIFY(!QFileInfo::exists(companionPath + QStringLiteral(".part")));
        ModelCatalog restored(directory.path(), true);
        QCOMPARE(restored.modelById(id)->companionFileName, model->companionFileName);
        QCOMPARE(restored.modelById(id)->companionRole, model->companionRole);
        QCOMPARE(restored.models(category).size(), 1);
    }
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration);
    QString startError; QVERIFY2(service.start(&startError), qPrintable(startError));
    QWebSocket socket;
    QJsonArray listedCategories;
    connect(&socket, &QWebSocket::textMessageReceived, this, [&socket, &listedCategories](const QString &text) {
        const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
        if (message.value(QStringLiteral("type")) == QStringLiteral("authenticated")) {
            socket.sendTextMessage(QStringLiteral("{\"type\":\"models.list\"}"));
        } else if (message.value(QStringLiteral("type")) == QStringLiteral("models.list.result")) {
            listedCategories = message.value(QStringLiteral("categories")).toArray();
        }
    });
    socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(service.serverPort())));
    QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
    socket.sendTextMessage(QStringLiteral("{\"type\":\"authenticate\",\"version\":3}"));
    QTRY_VERIFY(!listedCategories.isEmpty());
    int pairedCategories = 0;
    for (const auto &value : listedCategories) {
        const QJsonObject category = value.toObject();
        const auto models = category.value(QStringLiteral("models")).toArray();
        if (models.isEmpty()) continue;
        ++pairedCategories;
        const auto model = models.first().toObject();
        const QString role = category.value(QStringLiteral("category")) == QStringLiteral("llm-chat")
            ? QStringLiteral("mmproj") : QStringLiteral("vae");
        QCOMPARE(model.value(QStringLiteral("companionRole")).toString(), role);
        QVERIFY(model.value(QStringLiteral("companionFileName")).toString().startsWith(QStringLiteral("companions/shared.gguf/")));
        QVERIFY(!model.contains(QStringLiteral("filePath")));
    }
    QCOMPARE(pairedCategories, 3);
    socket.close();
    for (const ModelCategory category : {ModelCategory::LlmChat, ModelCategory::ImageGeneration, ModelCategory::Edition}) {
        const QString id = ModelCatalog::categoryName(category) + QStringLiteral(":shared.gguf");
        const auto model = catalog.modelById(id);
        const QString companionPath = QDir(QFileInfo(model->filePath).absolutePath()).filePath(model->companionFileName);
        QString error; QVERIFY(catalog.remove(id, &error));
        QVERIFY(!QFileInfo::exists(companionPath));
        QVERIFY(!catalog.modelById(id).has_value());
    }
}

void ServiceTest::rollsBackFailedCompanion()
{
    TestHttpServer server(QByteArray("model-bytes")); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy failed(&catalog, &ModelCatalog::installFailed);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    QString error;
    QVERIFY(catalog.install({QStringLiteral("failed-pair"), ModelCategory::Edition,
        server.url(), server.url(QStringLiteral("missing.safetensors"))}, &error));
    QTRY_COMPARE(failed.count(), 1);
    const QString errorMessage = failed.first().at(1).toString();
    QVERIFY(errorMessage.contains(QStringLiteral("HTTP 404")));
    QVERIFY(errorMessage.contains(QStringLiteral("vae missing.safetensors")));
    QCOMPARE(finished.count(), 0);
    QVERIFY(catalog.models(ModelCategory::Edition).isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("edition/model.gguf"))));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("edition/model.gguf.part"))));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("edition/companions/model.gguf/missing.safetensors.part"))));
}

void ServiceTest::acceptsExternalModelUrls()
{
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true);
    QString error;
    const ModelInstallRequest request{QStringLiteral("external-1"), ModelCategory::LlmChat,
                                      QUrl(QStringLiteral("https://models.example.test/releases/chat-model.gguf")), {}};
    QVERIFY(catalog.install(request, &error));
    QVERIFY(error.isEmpty());
    catalog.cancel(request.installId);
}

void ServiceTest::removesModelByOpaqueId()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("upscale"))));
    QFile model(directory.filePath(QStringLiteral("upscale/two-x.gguf"))); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
    ModelCatalog catalog(directory.path(), true);
    const std::optional<ModelInfo> info = catalog.modelById(QStringLiteral("upscaler:two-x.gguf"));
    QVERIFY(info.has_value()); QCOMPARE(info->fileName, QStringLiteral("two-x.gguf"));
    QString error; QVERIFY(catalog.remove(QStringLiteral("upscaler:two-x.gguf"), &error)); QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo::exists(model.fileName()));
    QVERIFY(!catalog.remove(QStringLiteral("upscaler:two-x.gguf"), &error)); QVERIFY(!error.isEmpty());
    QVERIFY(!catalog.remove(QStringLiteral("upscaler:../two-x.gguf"), &error));
}

void ServiceTest::rejectsInvalidModelInstall()
{
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true); QString error;
    QVERIFY(!catalog.install({QStringLiteral("bad"), ModelCategory::Upscaler, QUrl(QStringLiteral("http://127.0.0.1:1/owner/repo/resolve/main/model.bin")), {}}, &error));
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(!catalog.install({QStringLiteral("bad-companion"), ModelCategory::IpAdapter,
                              QUrl(QStringLiteral("https://models.example.test/ip-adapter.bin")),
                              QUrl(QStringLiteral("https://models.example.test/companion.txt"))}, &error));
    QVERIFY(!error.isEmpty());
    for (const ModelCategory category : {ModelCategory::LlmChat, ModelCategory::Upscaler}) {
        QVERIFY(!catalog.install({QStringLiteral("wrong-companion"), category,
            QUrl(QStringLiteral("https://example.test/model.gguf")),
            QUrl(QStringLiteral("https://example.test/vae.safetensors"))}, &error));
    }
}

void ServiceTest::listsSupportedModelsWithoutPathLeak()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("upscale"))));
    QFile generation(directory.filePath(QStringLiteral("model.safetensors"))); QVERIFY(generation.open(QIODevice::WriteOnly)); generation.write("model"); generation.close();
    QFile upscaler(directory.filePath(QStringLiteral("upscale/two-x.gguf"))); QVERIFY(upscaler.open(QIODevice::WriteOnly)); upscaler.write("model"); upscaler.close();
    QFile ignored(directory.filePath(QStringLiteral("ignored.txt"))); QVERIFY(ignored.open(QIODevice::WriteOnly)); ignored.close();
    ModelCatalog catalog(directory.path(), true);
    const QList<ModelInfo> models = catalog.models(ModelCategory::ImageGeneration);
    QCOMPARE(models.size(), 1); QCOMPARE(models.first().id, QStringLiteral("image-generation:model.safetensors"));
    QVERIFY(!models.first().id.contains(directory.path())); QCOMPARE(catalog.models(ModelCategory::Upscaler).size(), 1);
}

void ServiceTest::migratesLegacyModelRoot()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString legacy = ServicePaths::legacyModelsPath(); const QString destination = ServicePaths::modelsPath();
    QDir(QFileInfo(legacy).dir().absolutePath()).removeRecursively(); QDir(QFileInfo(destination).dir().absolutePath()).removeRecursively();
    QVERIFY(QDir().mkpath(legacy)); QFile model(QDir(legacy).filePath(QStringLiteral("existing.safetensors"))); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
    QString error; QVERIFY(ServicePaths::migrateLegacyModels(&error)); QVERIFY(QFileInfo::exists(QDir(destination).filePath(QStringLiteral("existing.safetensors")))); QVERIFY(!QFileInfo::exists(legacy));
    QDir(QFileInfo(destination).dir().absolutePath()).removeRecursively();
}

void ServiceTest::rejectsConflictingModelRoots()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString legacy = ServicePaths::legacyModelsPath(); const QString destination = ServicePaths::modelsPath();
    QDir(QFileInfo(legacy).dir().absolutePath()).removeRecursively(); QDir(QFileInfo(destination).dir().absolutePath()).removeRecursively();
    QVERIFY(QDir().mkpath(legacy)); QVERIFY(QDir().mkpath(destination));
    QString error; QVERIFY(!ServicePaths::migrateLegacyModels(&error)); QVERIFY(!error.isEmpty()); QVERIFY(QFileInfo::exists(legacy)); QVERIFY(QFileInfo::exists(destination));
    QDir(QFileInfo(legacy).dir().absolutePath()).removeRecursively(); QDir(QFileInfo(destination).dir().absolutePath()).removeRecursively();
}

QTEST_MAIN(ServiceTest)
#include "ServiceTest.moc"
