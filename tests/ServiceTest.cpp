#include "KiraService.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include <kira/service/ServiceProtocol.h>
#include <kira/service/ModelOptions.h>

#include <QDir>
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
