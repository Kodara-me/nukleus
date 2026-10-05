#include "KiraService.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include <kira/service/ServiceProtocol.h>
#include <kira/service/ModelOptions.h>
#include <kira/service/ServiceClient.h>

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
    void persistsPromptStyles();
    void installsAndPersistsPromptEnhancer();
    void installsAndPersistsFaceParsing();
    void generatesFaceMaskThroughSdk();
    void inpaintsThroughSdk();
    void enhancesPromptThroughSdk();
    void configuresStylesOverProtocol();
    void generatesWithValidatedOptionsAndVae();
    void validatesRecommendedOptions();
    void persistsOptionsAndLegacyMetadata();
    void reservesSingleInstallAndCancels();
    void cancelsDuringCompanionDownload();
    void rollsBackMetadataFailure();
    void configuresModelsOverProtocol();
    void preservesProtocolV3();
    void importsLocalModelWithoutChangingSources();
    void rejectsLanWithoutToken();
    void startsOnLoopbackAndLanAddress();
    void authenticatesLoopbackWithoutToken();
    void rejectsLanAuthenticationWithoutToken();
    void rollsBackWhenLanListenerFails();
    void listsSupportedModelsWithoutPathLeak();
    void removesModelByOpaqueId();
    void installsModelAtomically();
    void installsIpAdapterPair();
    void installsClipVision();
    void installsPhotoMaker();
    void exportsPhotoMakerIdentity();
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

void ServiceTest::inpaintsThroughSdk()
{
#ifndef Q_OS_UNIX
    QSKIP("The disposable backend uses a POSIX shell");
#else
    QTemporaryDir directory;
    QVERIFY(QDir().mkpath(directory.filePath("models/inpaint")));
    QVERIFY(QDir().mkpath(directory.filePath("models/clip_vision")));
    QVERIFY(QDir().mkpath(directory.filePath("models/ip-adapter")));
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(directory.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAIAAAAlC+aJAAAAfElEQVR4nNXOQREAMAjAsK7+PTMRPLhGQd7QJnESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ESJ3ES53Vg6wNShQF/fRSLfgAAAABJRU5ErkJggg==");
    QVERIFY(write("fixture.png", png));
    QVERIFY(write("models/inpaint/inpaint.safetensors", "weights"));
    QVERIFY(write("models/clip_vision/vision.safetensors", "vision weights"));
    QVERIFY(write("models/ip-adapter/adapter.safetensors", "adapter weights"));
    const auto root = directory.path().toUtf8();
    QVERIFY(write("sd-cli", "#!/bin/sh\n"
        "if [ \"$1\" = --help ]; then exit 0; fi\n"
        "if [ -f '" + root + "/fail' ]; then echo 'inpaint fixture failure' >&2; exit 1; fi\n"
        "printf '%s\\n' \"$@\" > '" + root + "/args'\n"
        "previous=''\nfor argument in \"$@\"; do\n"
        "if [ \"$previous\" = --init-img ]; then cp \"$argument\" '" + root + "/source.png'; fi\n"
        "if [ \"$previous\" = --mask ]; then cp \"$argument\" '" + root + "/mask.png'; fi\n"
        "if [ \"$previous\" = --ip-adapter-image ]; then cp \"$argument\" '" + root + "/reference.png'; fi\n"
        "if [ \"$previous\" = --output ]; then cp '" + root + "/fixture.png' \"$argument\"; fi\n"
        "previous=\"$argument\"\ndone\n"));
    QVERIFY(QFile::setPermissions(directory.filePath("sd-cli"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const auto oldPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
    qputenv("PATH", root + ':' + oldPath);
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = directory.filePath("models");
    KiraServiceDaemon daemon(configuration);
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    client.connectToService();
    QTRY_VERIFY(client.canSubmit(KiraService::ImageOperation::Inpaint, "inpaint:inpaint.safetensors"));
    KiraService::ImageGenerationRequest request;
    request.modelId = "inpaint:inpaint.safetensors";
    request.prompt = "adult with green eyes";
    request.negativePrompt = "blur";
    request.width = request.height = 64;
    request.steps = 5;
    request.seed = 456;
    request.baseImageStrength = 0.99;
    request.baseImageData = png;
    QSignalSpy finished(&client, &KiraService::ServiceClient::imageJobFinished);
    QVERIFY(!client.submitInpaint(request, png).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    const auto result = qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0));
    QVERIFY(result.succeeded);
    QCOMPARE(result.operation, KiraService::ImageOperation::Inpaint);
    QCOMPARE(result.imageData, png);
    const auto read = [&](const QString& name) {
        QFile file(directory.filePath(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    QCOMPARE(read("source.png"), png);
    QCOMPARE(read("mask.png"), png);
    const auto arguments = QString::fromUtf8(read("args")).split('\n');
    QVERIFY(arguments.contains("--mask"));
    QVERIFY(arguments.contains("--init-img"));
    QVERIFY(!arguments.contains("--ref-image"));
    QCOMPARE(arguments.value(arguments.indexOf("--strength") + 1).toDouble(), 0.99);
    QCOMPARE(arguments.value(arguments.indexOf("--seed") + 1), QString("456"));
    QCOMPARE(arguments.value(arguments.indexOf("--negative-prompt") + 1), QString("blur"));
    QVERIFY(client.submitInpaint(request, {}).isEmpty());
    auto invalid = request;
    invalid.width = 128;
    QVERIFY(!client.submitInpaint(invalid, png).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0)).errorCode, QString("invalid-image"));
    invalid = request;
    invalid.photoMakerModelId = "identity";
    QVERIFY(client.submitInpaint(invalid, png).isEmpty());
    QVERIFY(write("fail", ""));
    QVERIFY(!client.submitInpaint(request, png).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    const auto failure = qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0));
    QVERIFY(!failure.succeeded);
    QVERIFY(failure.message.contains("inpaint fixture failure"));
    QVERIFY(QFile::remove(directory.filePath("fail")));
    QVERIFY(!client.submitInpaint(request, png).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0)).succeeded);
    // Adapter reference is a third upload, independent from source/mask dimensions.
    const QByteArray reference = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1kAAAAASUVORK5CYII=");
    request.clipVisionModelId = "clip_vision:vision.safetensors";
    request.ipAdapterModelId = "ip-adapter:adapter.safetensors";
    request.ipAdapterImageData = reference;
    request.ipAdapterStrength = 0.8;
    QVERIFY(!client.submitInpaint(request, png).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0)).succeeded);
    QCOMPARE(read("source.png"), png);
    QCOMPARE(read("mask.png"), png);
    QCOMPARE(read("reference.png"), reference);
    const auto adapterArguments = QString::fromUtf8(read("args")).split('\n');
    QCOMPARE(adapterArguments.value(adapterArguments.indexOf("--clip_vision") + 1),
        directory.filePath("models/clip_vision/vision.safetensors"));
    QCOMPARE(adapterArguments.value(adapterArguments.indexOf("--ip-adapter") + 1),
        directory.filePath("models/ip-adapter/adapter.safetensors"));
    QVERIFY(adapterArguments.contains("--ip-adapter-image"));
    QCOMPARE(adapterArguments.value(adapterArguments.indexOf("--ip-adapter-strength") + 1).toDouble(), 0.8);
    request.clipVisionModelId = "missing";
    QVERIFY(client.submitInpaint(request, png).isEmpty());
#endif
}

void ServiceTest::generatesFaceMaskThroughSdk()
{
#ifndef Q_OS_UNIX
    QSKIP("The disposable process fixture uses a POSIX shell");
#else
    QTemporaryDir directory;
    QVERIFY(QDir().mkpath(directory.filePath("models/face-parsing")));
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(directory.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QVERIFY(write("models/face-parsing/parser.onnx", "fixture"));
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1kAAAAASUVORK5CYII=");
    QVERIFY(write("fixture.png", png));
    const auto path = directory.path().toUtf8();
    QVERIFY(write("worker", "#!/bin/sh\n"
        "printf '%s' \"$6\" > '" + path + "/options.json'\n"
        "if [ -f '" + path + "/fail' ]; then echo 'fixture parsing error' >&2; exit 1; fi\n"
        "if [ -f '" + path + "/slow' ]; then sleep 2; fi\n"
        "cp '" + path + "/fixture.png' \"$5\"\n"));
    QVERIFY(QFile::setPermissions(directory.filePath("worker"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    auto options = KiraService::Protocol::defaultModelOptions("face-parsing");
    options.insert("dilatePixels", 11);
    ModelCatalog catalog(directory.filePath("models"), false);
    QVERIFY(catalog.configure("face-parsing:parser.onnx", options));
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = directory.filePath("models");
    configuration.faceParsingPython = directory.filePath("worker");
    configuration.faceParsingTimeoutMs = 100;
    KiraServiceDaemon daemon(configuration);
    QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    client.connectToService();
    QTRY_VERIFY(client.canSubmit(KiraService::ImageOperation::FaceParsing, "face-parsing:parser.onnx"));
    QSignalSpy finished(&client, &KiraService::ServiceClient::imageJobFinished);
    const auto submit = [&] { return client.submitFaceParsing({png, "face-parsing:parser.onnx"}); };
    QVERIFY(!submit().isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    auto result = qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0));
    QVERIFY(result.succeeded);
    QCOMPARE(result.operation, KiraService::ImageOperation::FaceParsing);
    QCOMPARE(result.imageData, png);
    QFile captured(directory.filePath("options.json"));
    QVERIFY(captured.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(captured.readAll()).object(), options);
    QVERIFY(client.submitFaceParsing({{}, "face-parsing:parser.onnx"}).isEmpty());
    QVERIFY(write("fail", ""));
    QVERIFY(!submit().isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    result = qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0));
    QVERIFY(!result.succeeded);
    QCOMPARE(result.errorCode, QString("face-parsing-failed"));
    QVERIFY(result.message.contains("fixture parsing error"));
    QVERIFY(QFile::remove(directory.filePath("fail")));
    QVERIFY(write("slow", ""));
    QVERIFY(!submit().isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    result = qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0));
    QVERIFY(!result.succeeded);
    QVERIFY(result.message.contains("timed out"));
    QVERIFY(QFile::remove(directory.filePath("slow")));
    QVERIFY(!submit().isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0)).succeeded);
    // A disconnected active job must release the worker and the shared queue.
    QVERIFY(write("slow", ""));
    QSignalSpy states(&client, &KiraService::ServiceClient::jobStateChanged);
    const auto disconnectedJob = submit();
    QVERIFY(!disconnectedJob.isEmpty());
    QTRY_VERIFY(std::any_of(states.cbegin(), states.cend(), [&](const auto& event) {
        return event.at(0).toString() == disconnectedJob
            && qvariant_cast<KiraService::JobState>(event.at(1)) == KiraService::JobState::Running;
    }));
    client.disconnectFromService();
    QVERIFY(QFile::remove(directory.filePath("slow")));
    KiraService::ServiceClient reconnected;
    reconnected.setConfiguration(client.endpoint(), {});
    reconnected.connectToService();
    QTRY_VERIFY(reconnected.canSubmit(KiraService::ImageOperation::FaceParsing, "face-parsing:parser.onnx"));
    QSignalSpy recovered(&reconnected, &KiraService::ServiceClient::imageJobFinished);
    QVERIFY(!reconnected.submitFaceParsing({png, "face-parsing:parser.onnx"}).isEmpty());
    QTRY_COMPARE(recovered.count(), 1);
    QVERIFY(qvariant_cast<KiraService::ImageJobResult>(recovered.first().at(0)).succeeded);
#endif
}

void ServiceTest::installsAndPersistsFaceParsing()
{
    using namespace KiraService::Protocol;
    TestHttpServer server("onnx weights");
    QVERIFY(server.start());
    QTemporaryDir directory;
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy completed(&catalog, &ModelCatalog::installFinished);
    QString error;
    auto options = defaultModelOptions("face-parsing");
    QVERIFY(validateModelOptions("face-parsing", options));
    QCOMPARE(options.value("includedClasses").toArray().size(), 12);
    QVERIFY(!options.value("includedClasses").toArray().contains("neck"));
    QCOMPARE(options.value("dilatePixels").toInt(), 8);
    QCOMPARE(options.value("featherPixels").toInt(), 8);
    ModelInstallRequest request;
    request.installId = "face-parser";
    request.category = ModelCategory::FaceParsing;
    request.url = server.url("model.onnx");
    request.recommendedOptions = options;
    request.companionUrl = server.url("vae.gguf");
    QVERIFY(!catalog.install(request, &error));
    request.companionUrl.reset();
    request.url = server.url("model.gguf");
    QVERIFY(!catalog.install(request, &error));
    request.url = server.url("model.onnx");
    QVERIFY(catalog.install(request, &error));
    QTRY_COMPARE(completed.count(), 1);
    const QString id = "face-parsing:model.onnx";
    QVERIFY(catalog.modelById(id));
    QVERIFY(QFileInfo::exists(directory.filePath("face-parsing/model.onnx")));
    QCOMPARE(catalog.modelById(id)->recommendedOptions, options);
    QVERIFY(catalog.models(ModelCategory::ImageGeneration).isEmpty());
    options.insert("includedClasses", QJsonArray{"skin", "hair"});
    options.insert("dilatePixels", 12);
    options.insert("invertMask", true);
    QVERIFY(catalog.configure(id, options, &error, true, QString("Face parser")));
    ModelCatalog restarted(directory.path(), true);
    QCOMPARE(restarted.modelById(id)->recommendedOptions, options);
    QVERIFY(restarted.modelById(id)->explicitContent);
    QCOMPARE(restarted.modelById(id)->displayName, QString("Face parser"));
    QCOMPARE(ModelCatalog::categoryFromName("face-parsing"), std::optional<ModelCategory>(ModelCategory::FaceParsing));
    QVERIFY(ModelCatalog::companionRole(ModelCategory::FaceParsing).isEmpty());
    for (const auto& invalid : {QJsonArray{}, QJsonArray{"hair", "hair"}, QJsonArray{"left_eye"}, QJsonArray{1}}) {
        auto bad = options;
        bad.insert("includedClasses", invalid);
        QVERIFY(!catalog.configure(id, bad, &error));
        QCOMPARE(catalog.modelById(id)->recommendedOptions, options);
    }
    for (const auto& key : {"dilatePixels", "featherPixels"}) {
        for (const auto& invalid : {QJsonValue(-1), QJsonValue(257), QJsonValue(1.5), QJsonValue("8")}) {
            auto bad = options;
            bad.insert(key, invalid);
            QVERIFY(!validateModelOptions("face-parsing", bad));
        }
    }
    for (const auto& key : {"invertMask", "preserveAspectRatio", "outputAtSourceResolution", "provider"}) {
        auto bad = options;
        bad.insert(key, 1);
        QVERIFY(!validateModelOptions("face-parsing", bad));
    }
    auto bad = options;
    bad.insert("provider", "CUDAExecutionProvider");
    QVERIFY(!validateModelOptions("face-parsing", bad));
    QVERIFY(catalog.remove(id, &error));
    QVERIFY(!QFileInfo::exists(directory.filePath("face-parsing/model.onnx")));
    QVERIFY(restarted.models(ModelCategory::FaceParsing).isEmpty());
}

void ServiceTest::persistsPromptStyles()
{
    QTemporaryDir directory;
    QFile model(directory.filePath("test.gguf"));
    QVERIFY(model.open(QIODevice::WriteOnly)); model.write("weights"); model.close();
    ModelCatalog catalog(directory.path(), false);
    const QString id = "image-generation:test.gguf";
    QCOMPARE(catalog.modelById(id)->styles.size(), 3);
    QVERIFY(!QFile::exists(directory.filePath("metadata.json")));
    QVERIFY(QDir().mkpath(directory.filePath("companions/test.gguf")));
    QFile companion(directory.filePath("companions/test.gguf/vae.gguf"));
    QVERIFY(companion.open(QIODevice::WriteOnly)); companion.write("vae"); companion.close();
    QFile initialMetadata(directory.filePath("metadata.json"));
    QVERIFY(initialMetadata.open(QIODevice::WriteOnly));
    initialMetadata.write(QJsonDocument(QJsonObject{{"test.gguf", "companions/test.gguf/vae.gguf"}, {"untouched.gguf", "legacy"}}).toJson());
    initialMetadata.close();
    QString error;
    const QList<KiraService::PromptStyle> styles{{"custom", "Custom", " light ", " blur "}};
    QVERIFY(catalog.configureStyles(id, styles, &error));
    ModelCatalog restarted(directory.path(), false);
    QCOMPARE(restarted.modelById(id)->styles, styles);
    QCOMPARE(restarted.modelById(id)->companionFileName, QString("companions/test.gguf/vae.gguf"));
    auto invalid = styles; invalid.append(styles.first());
    QVERIFY(!catalog.configureStyles(id, invalid, &error));
    QCOMPARE(catalog.modelById(id)->styles, styles);
    auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    options.insert("steps", 30);
    QVERIFY(catalog.configure(id, options, &error, true));
    QCOMPARE(catalog.modelById(id)->styles, styles);
    QVERIFY(catalog.configureStyles(id, {}, &error));
    QCOMPARE(restarted.modelById(id)->styles.size(), 0);
    QCOMPARE(restarted.modelById(id)->recommendedOptions.value("steps").toInt(), 30);
    QVERIFY(restarted.modelById(id)->explicitContent);
    QFile savedMetadata(directory.filePath("metadata.json"));
    QVERIFY(savedMetadata.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(savedMetadata.readAll()).object().value("untouched.gguf").toString(), QString("legacy"));
    savedMetadata.close();
#ifdef Q_OS_UNIX
    // Block QSaveFile's temporary-file creation while keeping metadata readable.
    const auto permissions = QFileInfo(directory.path()).permissions();
    const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(directory.path(), permissions); });
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
    QFile probe(directory.filePath("write-probe"));
    if (!probe.open(QIODevice::WriteOnly)) {
        QVERIFY(!catalog.configureStyles(id, styles, &error));
        QVERIFY(error.contains("save"));
        QVERIFY(restarted.modelById(id)->styles.isEmpty());
        QCOMPARE(restarted.modelById(id)->companionFileName, QString("companions/test.gguf/vae.gguf"));
    }
    probe.close();
    QVERIFY(QFile::setPermissions(directory.path(), permissions));
#endif
    QVERIFY(!catalog.configureStyles("edition:missing.gguf", styles, &error));
    QFile metadata(directory.filePath("metadata.json"));
    QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)); metadata.write("invalid"); metadata.close();
    QVERIFY(!catalog.configureStyles(id, styles, &error));
}

void ServiceTest::configuresStylesOverProtocol()
{
    QTemporaryDir directory;
    QFile model(directory.filePath("test.gguf"));
    QVERIFY(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration);
    QString error; QVERIFY2(service.start(&error), qPrintable(error));
    KiraService::ServiceClient owner, observer;
    const QUrl endpoint(QString("ws://127.0.0.1:%1").arg(service.serverPort()));
    owner.setConfiguration(endpoint, {}); observer.setConfiguration(endpoint, {});
    owner.connectToService(); observer.connectToService();
    QTRY_VERIFY(owner.modelListAvailable() && observer.modelListAvailable());
    QCOMPARE(observer.models(KiraService::ModelCategory::ImageGeneration).first().styles.size(), 3);
    QSignalSpy completed(&owner, &KiraService::ServiceClient::modelStylesConfigured);
    QSignalSpy failed(&owner, &KiraService::ServiceClient::modelOperationFailed);
    const QList<KiraService::PromptStyle> styles{{"custom", "Custom", "positive", ""}};
    QVERIFY(owner.configureModelStyles("image-generation:test.gguf", styles));
    QTRY_COMPARE(completed.size(), 1);
    QTRY_COMPARE(observer.models(KiraService::ModelCategory::ImageGeneration).first().styles, styles);
    QVERIFY(owner.configureModelStyles("edition:test.gguf", styles));
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(observer.models(KiraService::ModelCategory::ImageGeneration).first().styles, styles);
    QVERIFY(owner.configureModelStyles("image-generation:test.gguf", {}));
    QTRY_COMPARE(completed.size(), 2);
    QTRY_VERIFY(observer.models(KiraService::ModelCategory::ImageGeneration).first().styles.isEmpty());
    const auto validStyles = KiraService::Protocol::promptStylesToJson(styles);
    const QList<QJsonValue> invalidLists{QJsonObject{}, QJsonArray{validStyles.first(), validStyles.first()},
        QJsonArray{QJsonObject{{"id", "invalid"}, {"name", "Invalid"}, {"positivePrefix", true}, {"negativePrefix", ""}}}};
    auto *socket = owner.findChild<QWebSocket *>();
    QVERIFY(socket);
    int failureCount = failed.size();
    for (const auto &invalid : invalidLists) {
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.styles.configure"},
            {"id", "image-generation:test.gguf"}, {"styles", invalid}}).toJson()));
        ++failureCount;
        QTRY_COMPARE(failed.size(), failureCount);
        QVERIFY(observer.models(KiraService::ModelCategory::ImageGeneration).first().styles.isEmpty());
    }
}

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
    executable.write("#!/bin/sh\nif [ \"$KIRA_TEST_PM_FAIL\" = 1 ]; then printf 'Turn off PhotoMaker for this request' >&2; exit 0; fi\n"
                     "printf '%s\\n' \"$@\" > \"$KIRA_TEST_ARGS\"\nprevious=\"\"\nfor argument in "
                     "\"$@\"; do\n if [ \"$previous\" = --output ]; then cp \"$KIRA_TEST_IMAGE\" "
                     "\"$argument\"; fi\n if [ \"$previous\" = --ip-adapter-image ]; then cp \"$argument\" \"$KIRA_TEST_ARGS.ip\"; fi\n"
                     " if [ \"$previous\" = --pm-id-embed-path ]; then cp \"$argument\" \"$KIRA_TEST_ARGS.identity\"; fi\n"
                     " if [ \"$previous\" = --pm-id-images-dir ]; then cp \"$argument/input.png\" \"$KIRA_TEST_ARGS.reference\"; for n in 2 3 4; do if [ -f \"$argument/reference-$n.png\" ]; then cp \"$argument/reference-$n.png\" \"$KIRA_TEST_ARGS.reference-$n\"; fi; done; fi\n"
                     " if [ \"$previous\" = --ref-image ]; then cp \"$argument\" \"$KIRA_TEST_ARGS.source\"; fi\n previous=\"$argument\"\ndone\nexit 0\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString root = directory.filePath("models");
    const QString companion = "companions/model.gguf/vae with space.safetensors";
    const QString clipL = "companions/model.gguf/clip_l.safetensors";
    const QString clipG = "companions/model.gguf/clip_g.safetensors";
    const QString llm = "companions/model.gguf/text encoder.gguf";
    const QString mmproj = "companions/model.gguf/mmproj.gguf";
    QVERIFY(QDir().mkpath(QDir(root).filePath("companions/model.gguf")));
    for (const QString &name : {QString("model.gguf"), QString("plain.gguf"), companion, clipL, clipG, llm, mmproj})
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
    const QString editionRoot = QDir(root).filePath("edition");
    QVERIFY(QDir().mkpath(QDir(editionRoot).filePath("companions/edit.gguf")));
    for (const QString& name : {QString("edit.gguf"), QString("llm.gguf"), QString("vae.safetensors"), QString("mmproj.gguf")}) {
        QFile file(QDir(editionRoot).filePath(name == "edit.gguf" ? name : "companions/edit.gguf/" + name));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
    }
    QFile editionMetadata(QDir(editionRoot).filePath("metadata.json"));
    QVERIFY(editionMetadata.open(QIODevice::WriteOnly));
    editionMetadata.write(QJsonDocument(QJsonObject{{"edit.gguf", QJsonObject{
        {"modelType", "diffusion"}, {"companionFiles", QJsonObject{
            {"llm", "companions/edit.gguf/llm.gguf"}, {"vae", "companions/edit.gguf/vae.safetensors"}, {"mmproj", "companions/edit.gguf/mmproj.gguf"}}}}}}).toJson());
    editionMetadata.close();
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = root;
    configuration.maximumImageBytes = 1024;
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
    QJsonObject editOptions = valid;
    editOptions.insert("modelId", "edition:edit.gguf");
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"operation", "image.edit"}, {"jobId", "edit-test"},
        {"options", editOptions}}).toJson()));
    const auto uploadRequested = [&] {
        for (const auto& message : messages)
            if (message.value("type") == "upload-required" && message.value("jobId") == "edit-test") return true;
        return false;
    };
    QTRY_VERIFY(uploadRequested());
    socket.sendBinaryMessage(image);
    QTRY_VERIFY(terminal("edit-test"));
    QTRY_COMPARE(images.count(), 1);
    const auto editArgs = arguments();
    QVERIFY(editArgs.contains("--ref-image"));
    QCOMPARE(editArgs.count("--offload-to-cpu"), 1);
    QVERIFY(editArgs.contains("--diffusion-fa"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--model-args") + 1), QString("qwen_image_zero_cond_t=true"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--flow-shift") + 1), QString("3"));
    QVERIFY(!editArgs.contains("--init-img"));
    QVERIFY(!editArgs.contains("--strength"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--diffusion-model") + 1), QDir(editionRoot).filePath("edit.gguf"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--llm") + 1), QDir(editionRoot).filePath("companions/edit.gguf/llm.gguf"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--vae") + 1), QDir(editionRoot).filePath("companions/edit.gguf/vae.safetensors"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--llm_vision") + 1), QDir(editionRoot).filePath("companions/edit.gguf/mmproj.gguf"));
    QCOMPARE(messages.last().value("operation").toString(), QString("image.edit"));
    const QString visionPath = QDir(root).filePath("clip_vision/vision.safetensors");
    const QString adapterPath = QDir(root).filePath("ip-adapter/adapter.safetensors");
    const QString auxiliaryPath = QDir(root).filePath("ip-adapter/companions/adapter.safetensors/lora/auxiliary.safetensors");
    for (const auto &path : {visionPath, adapterPath, auxiliaryPath}) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("weights");
    }
    QFile adapterMetadata(QDir(root).filePath("ip-adapter/metadata.json"));
    QVERIFY(adapterMetadata.open(QIODevice::WriteOnly));
    adapterMetadata.write(QJsonDocument(QJsonObject{{"adapter.safetensors", QJsonObject{
        {"companionFiles", QJsonObject{{"lora", "companions/adapter.safetensors/lora/auxiliary.safetensors"}}},
        {"recommendedOptions", QJsonObject{{"strength", 1.8}}}}}}).toJson());
    adapterMetadata.close();
    QJsonObject adapterOptions = valid;
    adapterOptions.insert("clipVisionModelId", "clip_vision:vision.safetensors");
    adapterOptions.insert("ipAdapterModelId", "ip-adapter:adapter.safetensors");
    adapterOptions.insert("hasIpAdapterImage", true);
    adapterOptions.insert("ip_adapter_strength", 0.8123456789);
    const QByteArray adapterImage("distinct IP-Adapter reference");
    const auto uploadFor = [&](const QString &id, const QString &role) {
        return std::any_of(messages.begin(), messages.end(), [&](const auto &message) {
            return message.value("type") == "upload-required" && message.value("jobId") == id
                && message.value("inputRole") == role;
        });
    };
    const auto capturedImage = [&](const QString &suffix) {
        QFile file(capture + suffix);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QString photoMakerPath = QDir(root).filePath("photomaker/identity.safetensors");
    QVERIFY(QDir().mkpath(QFileInfo(photoMakerPath).absolutePath()));
    QFile photoMakerFile(photoMakerPath);
    QVERIFY(photoMakerFile.open(QIODevice::WriteOnly)); photoMakerFile.write("weights"); photoMakerFile.close();
    QJsonObject photoMakerOptions = valid;
    photoMakerOptions.insert("photoMakerModelId", "photomaker:identity.safetensors");
    photoMakerOptions.insert("hasPhotoMakerIdentity", true);
    photoMakerOptions.insert("photoMakerStyleStrength", 31);
    photoMakerOptions.insert("styleId", "cinematic");
    const QByteArray identityBytes("opaque\0identity", 15);
    for (int pass = 0; pass < 2; ++pass) {
        const QString id = QString("photomaker-generate-%1").arg(pass);
        if (pass == 1) photoMakerOptions.insert("referenceCount", 4);
        images.clear(); messages.clear();
        send(id, photoMakerOptions);
        QTRY_VERIFY(uploadFor(id, "photomaker-reference"));
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.delete"},
            {"id", "photomaker:identity.safetensors"}}).toJson()));
        QTRY_COMPARE(messages.last().value("code").toString(), QString("model-in-use"));
        socket.sendBinaryMessage(image);
        if (pass == 1) {
            for (int i = 1; i < 4; ++i) {
                QTRY_VERIFY(messages.last().value("inputRole") == "photomaker-reference"
                    && messages.last().value("referenceIndex").toInt(-1) == i);
                socket.sendBinaryMessage(image + QByteArray::number(i));
            }
        }
        QTRY_VERIFY(uploadFor(id, "photomaker-identity"));
        socket.sendBinaryMessage(identityBytes);
        QTRY_VERIFY(terminal(id)); QTRY_COMPARE(images.count(), 1);
        const auto args = arguments();
        QCOMPARE(args.value(args.indexOf("--photo-maker") + 1), photoMakerPath);
        QCOMPARE(args.value(args.indexOf("--prompt") + 1), QString("cinematic lighting, dramatic composition, film still, test"));
        QCOMPARE(args.value(args.indexOf("--pm-style-strength") + 1), pass == 0 ? QString("31") : QString("20"));
        QCOMPARE(capturedImage(".identity"), identityBytes);
        QCOMPARE(capturedImage(".reference"), image);
        if (pass == 1) {
            for (int i = 1; i < 4; ++i)
                QCOMPARE(capturedImage(QStringLiteral(".reference-%1").arg(i + 1)), image + QByteArray::number(i));
        }
        QVERIFY(!args.contains("--init-img")); QVERIFY(!args.contains("--ip-adapter"));
        QVERIFY(!QFileInfo::exists(args.value(args.indexOf("--pm-id-embed-path") + 1)));
        photoMakerOptions.remove("photoMakerStyleStrength");
    }
    photoMakerOptions.remove("referenceCount");
    images.clear(); messages.clear();
    qputenv("KIRA_TEST_PM_FAIL", "1");
    send("photomaker-disabled", photoMakerOptions);
    QTRY_VERIFY(uploadFor("photomaker-disabled", "photomaker-reference"));
    socket.sendBinaryMessage(image);
    QTRY_VERIFY(uploadFor("photomaker-disabled", "photomaker-identity"));
    socket.sendBinaryMessage(identityBytes);
    QTRY_VERIFY(terminal("photomaker-disabled"));
    QCOMPARE(messages.last().value("type").toString(), QString("error"));
    QCOMPARE(images.count(), 0);
    qunsetenv("KIRA_TEST_PM_FAIL");
    for (int pass = 0; pass < 2; ++pass) {
        messages.clear();
        send("photomaker-empty", photoMakerOptions);
        QTRY_VERIFY(uploadFor("photomaker-empty", "photomaker-reference"));
        if (pass == 1) {
            socket.sendBinaryMessage(image);
            QTRY_VERIFY(uploadFor("photomaker-empty", "photomaker-identity"));
        }
        socket.sendBinaryMessage({});
        QTRY_VERIFY(terminal("photomaker-empty"));
        QCOMPARE(messages.last().value("code").toString(), QString("invalid-image"));
    }
    for (const QJsonObject change : {QJsonObject{{"photoMakerStyleStrength", 1.5}},
         QJsonObject{{"photoMakerStyleStrength", 101}}, QJsonObject{{"hasPhotoMakerIdentity", false}},
         QJsonObject{{"photoMakerModelId", "image-generation:model.gguf"}}, QJsonObject{{"hasInput", true}},
         QJsonObject{{"ipAdapterModelId", "ip-adapter:adapter.safetensors"}}}) {
        auto invalid = photoMakerOptions;
        for (auto it = change.begin(); it != change.end(); ++it) invalid.insert(it.key(), it.value());
        messages.clear(); send("invalid-photomaker", invalid);
        QTRY_VERIFY(terminal("invalid-photomaker"));
        QVERIFY(!uploadFor("invalid-photomaker", "photomaker-reference"));
    }
    images.clear(); messages.clear();
    adapterOptions.insert("styleId", "cinematic");
    send("adapter-generate", adapterOptions);
    QTRY_VERIFY(uploadFor("adapter-generate", "ip-adapter"));
    // All selected weights are protected while either reference upload is pending.
    for (const auto &id : {QString("clip_vision:vision.safetensors"), QString("ip-adapter:adapter.safetensors")}) {
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.delete"}, {"id", id}}).toJson()));
        QTRY_COMPARE(messages.last().value("code").toString(), QString("model-in-use"));
        messages.removeLast();
    }
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "models.companion.delete"}, {"id", "ip-adapter:adapter.safetensors"}, {"role", "lora"}}).toJson()));
    QTRY_COMPARE(messages.last().value("code").toString(), QString("model-in-use"));
    socket.sendBinaryMessage(adapterImage);
    QTRY_VERIFY(terminal("adapter-generate"));
    QTRY_COMPARE(images.count(), 1);
    auto adapterArgs = arguments();
    QCOMPARE(adapterArgs.value(adapterArgs.indexOf("--clip_vision") + 1), visionPath);
    QCOMPARE(adapterArgs.value(adapterArgs.indexOf("--ip-adapter") + 1), adapterPath);
    QCOMPARE(adapterArgs.value(adapterArgs.indexOf("--ip-adapter-strength") + 1).toDouble(), 0.8123456789);
    QCOMPARE(adapterArgs.value(adapterArgs.indexOf("--lora-model-dir") + 1), QFileInfo(auxiliaryPath).absolutePath());
    QCOMPARE(adapterArgs.value(adapterArgs.indexOf("--prompt") + 1), QString("cinematic lighting, dramatic composition, film still, test <lora:auxiliary.safetensors:1>"));
    QVERIFY(!adapterArgs.contains("--init-img")); QVERIFY(!adapterArgs.contains("--ref-image"));
    QCOMPARE(capturedImage(".ip"), adapterImage);

    images.clear(); messages.clear();
    adapterOptions.remove("styleId");
    adapterOptions.insert("modelId", "edition:edit.gguf");
    adapterOptions.remove("ip_adapter_strength");
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"operation", "image.edit"}, {"jobId", "adapter-edit"}, {"options", adapterOptions}}).toJson()));
    QTRY_VERIFY(uploadFor("adapter-edit", "source"));
    socket.sendBinaryMessage(image);
    QTRY_VERIFY(uploadFor("adapter-edit", "ip-adapter"));
    QVERIFY(!terminal("adapter-edit"));
    socket.sendBinaryMessage(adapterImage);
    QTRY_VERIFY(terminal("adapter-edit"));
    QTRY_COMPARE(images.count(), 1);
    adapterArgs = arguments();
    QVERIFY(adapterArgs.contains("--ref-image")); QVERIFY(adapterArgs.contains("--ip-adapter-image"));
    QVERIFY(!adapterArgs.contains("--init-img"));
    QVERIFY(adapterArgs.value(adapterArgs.indexOf("--ref-image") + 1)
        != adapterArgs.value(adapterArgs.indexOf("--ip-adapter-image") + 1));
    QCOMPARE(capturedImage(".source"), image); QCOMPARE(capturedImage(".ip"), adapterImage);
    QCOMPARE(adapterArgs.value(adapterArgs.indexOf("--ip-adapter-strength") + 1), QString("1"));
    // Recommendations never silently change explicit runtime selection or strength.
    adapterOptions.insert("modelId", "image-generation:model.gguf");
    int invalidAdapterIndex = 0;
    for (const auto &change : QList<QPair<QString, QJsonValue>>{
        {"clipVisionModelId", "edition:edit.gguf"}, {"ipAdapterModelId", "image-generation:model.gguf"},
        {"clipVisionModelId", 1}, {"ipAdapterModelId", "ip-adapter:missing.safetensors"},
        {"hasIpAdapterImage", false}, {"hasIpAdapterImage", "true"},
        {"ip_adapter_strength", "1"}, {"ip_adapter_strength", -0.1}, {"ip_adapter_strength", 2.1}}) {
        auto invalidOptions = adapterOptions; invalidOptions.insert(change.first, change.second);
        const auto id = QString("invalid-adapter-%1").arg(++invalidAdapterIndex);
        messages.clear(); send(id, invalidOptions); QTRY_VERIFY(terminal(id));
        QCOMPARE(messages.last().value("type").toString(), QString("error"));
        QVERIFY(!uploadFor(id, "ip-adapter"));
    }
    messages.clear(); send("adapter-empty", adapterOptions);
    QTRY_VERIFY(uploadFor("adapter-empty", "ip-adapter"));
    socket.sendBinaryMessage(QByteArray()); QTRY_VERIFY(terminal("adapter-empty"));
    QCOMPARE(messages.last().value("code").toString(), QString("invalid-image"));
    messages.clear(); send("adapter-oversized", adapterOptions);
    QTRY_VERIFY(uploadFor("adapter-oversized", "ip-adapter"));
    socket.sendBinaryMessage(QByteArray(1025, 'x')); QTRY_VERIFY(terminal("adapter-oversized"));
    QCOMPARE(messages.last().value("code").toString(), QString("invalid-image"));
    messages.clear(); send("adapter-pending", adapterOptions);
    QTRY_VERIFY(uploadFor("adapter-pending", "ip-adapter"));
    send("concurrent-upload", adapterOptions); QTRY_VERIFY(terminal("concurrent-upload"));
    QCOMPARE(messages.last().value("code").toString(), QString("upload-in-progress"));
    socket.sendBinaryMessage(adapterImage); QTRY_VERIFY(terminal("adapter-pending"));
    messages.clear();
    adapterOptions.insert("modelId", "edition:edit.gguf");
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"operation", "image.edit"}, {"jobId", "adapter-disconnect"}, {"options", adapterOptions}}).toJson()));
    QTRY_VERIFY(uploadFor("adapter-disconnect", "source")); socket.sendBinaryMessage(image);
    QTRY_VERIFY(uploadFor("adapter-disconnect", "ip-adapter"));
    socket.close(); QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
    messages.clear(); socket.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())));
    QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
    socket.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}"); QTRY_VERIFY(messages.size() >= 2);
    messages.clear();
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "models.delete"}, {"id", "clip_vision:vision.safetensors"}}).toJson()));
    QTRY_VERIFY(!messages.isEmpty());
    QTRY_COMPARE(messages.last().value("type").toString(), QString("models.delete.completed"));
    QVERIFY(!QFileInfo::exists(visionPath)); // Incomplete disconnected uploads released the selected weights.
    images.clear();
    messages.clear();
    send("automatic", valid);
    QTRY_VERIFY(terminal("automatic"));
    QTRY_COMPARE(images.count(), 1);
    QCOMPARE(images.last().first().toByteArray(), image);
    auto args = arguments();
    QCOMPARE(args.count("--offload-to-cpu"), 1);
    QVERIFY(!args.contains("--ip-adapter"));
    QVERIFY(!args.contains("--clip_vision"));
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
            {"vae", companion}, {"clip-l", clipL}, {"clip-g", clipG}, {"llm", llm}, {"mmproj", mmproj}}}}}}).toJson());
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
    QCOMPARE(args.value(args.indexOf("--llm") + 1), QDir(root).filePath(llm));
    QCOMPARE(args.value(args.indexOf("--llm_vision") + 1), QDir(root).filePath(mmproj));
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
    QVERIFY(!args.contains("--ip-adapter"));
    QVERIFY(!args.contains("--clip_vision"));
    QVERIFY(!args.contains("--sampling-method"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("7"));
    QCOMPARE(args.value(args.indexOf("--seed") + 1), QString("-1"));
    for (const auto &message : messages)
        QVERIFY(!QJsonDocument(message).toJson().contains(root.toUtf8()));
    messages.clear();
    send("defaults", {{"modelId", "image-generation:plain.gguf"}, {"prompt", "test"}});
    QTRY_VERIFY(terminal("defaults"));
    QTRY_COMPARE(images.count(), 5);
    args = arguments();
    QCOMPARE(args.value(args.indexOf("--width") + 1), QString("1024"));
    QCOMPARE(args.value(args.indexOf("--height") + 1), QString("1024"));
    QCOMPARE(args.value(args.indexOf("--steps") + 1), QString("28"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("7"));
    QVERIFY(!args.contains("--vae"));
    QVERIFY(!args.contains("--clip_l"));
    QVERIFY(!args.contains("--llm"));
    QVERIFY(!args.contains("--llm_vision"));
    QVERIFY(!args.contains("--clip_g"));
    QVERIFY(!args.contains("--ip-adapter"));
    QVERIFY(!args.contains("--clip_vision"));
    QVERIFY(!args.contains("--sampling-method"));
    QVERIFY(!args.contains("--scheduler"));
    int completed = 5;
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
    // Snapshot a selected style before upload, then delete it while the job waits.
    messages.clear();
    send("styled-snapshot", {{"modelId", "image-generation:plain.gguf"}, {"prompt", "  subject  "},
        {"negativePrompt", "  noise  "}, {"styleId", "photographic"}, {"hasInput", true}});
    const auto snapshotUploadRequested = [&] {
        for (const auto &message : messages)
            if (message.value("jobId") == "styled-snapshot" && message.value("type") == "upload-required") return true;
        return false;
    };
    QTRY_VERIFY(snapshotUploadRequested());
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.styles.configure"},
        {"id", "image-generation:plain.gguf"}, {"styles", QJsonArray{}}}).toJson()));
    const auto snapshotStyleSaved = [&] {
        for (const auto &message : messages)
            if (message.value("type") == "models.styles.configure.completed") return true;
        return false;
    };
    QTRY_VERIFY(snapshotStyleSaved());
    socket.sendBinaryMessage(image);
    QTRY_VERIFY(terminal("styled-snapshot"));
    ++completed;
    QTRY_COMPARE(images.count(), completed);
    args = arguments();
    QCOMPARE(args.value(args.indexOf("--prompt") + 1), QString("photorealistic, natural lighting, fine detail, subject"));
    QCOMPARE(args.value(args.indexOf("--negative-prompt") + 1), QString("illustration, painting, cartoon, noise"));
    messages.clear();
    send("deleted-style", {{"modelId", "image-generation:plain.gguf"}, {"prompt", "subject"}, {"styleId", "photographic"}});
    QTRY_VERIFY(terminal("deleted-style"));
    QCOMPARE(messages.last().value("code").toString(), QString("style-unavailable"));
    messages.clear();
    send("positive-style", {{"modelId", "image-generation:model.gguf"}, {"prompt", "subject"}, {"styleId", "cinematic"}});
    QTRY_VERIFY(terminal("positive-style"));
    ++completed;
    QTRY_COMPARE(images.count(), completed);
    args = arguments();
    QCOMPARE(args.value(args.indexOf("--prompt") + 1), QString("cinematic lighting, dramatic composition, film still, subject"));
    QCOMPARE(args.value(args.indexOf("--negative-prompt") + 1), QString());
    ModelCatalog styleCatalog(root, false);
    QVERIFY(styleCatalog.configureStyles("image-generation:plain.gguf", {{"negative", "Negative", "", "  blur  "}}, &error));
    messages.clear();
    send("negative-style", {{"modelId", "image-generation:plain.gguf"}, {"prompt", "  subject  "}, {"styleId", "negative"}});
    QTRY_VERIFY(terminal("negative-style"));
    ++completed;
    QTRY_COMPARE(images.count(), completed);
    args = arguments();
    QCOMPARE(args.value(args.indexOf("--prompt") + 1), QString("subject"));
    QCOMPARE(args.value(args.indexOf("--negative-prompt") + 1), QString("blur"));
    QVERIFY(styleCatalog.configureStyles("image-generation:plain.gguf", {{"lora", "LoRA", "<lora:custom:1>", ""}}, &error));
    // Recreate the disposable CLIP Vision weight deleted by the lifecycle test above.
    QFile restoredVision(visionPath);
    QVERIFY(restoredVision.open(QIODevice::WriteOnly)); restoredVision.write("weights"); restoredVision.close();
    auto invalidStyledAdapter = adapterOptions;
    invalidStyledAdapter.insert("modelId", "image-generation:plain.gguf");
    invalidStyledAdapter.insert("styleId", "lora");
    messages.clear(); send("styled-lora", invalidStyledAdapter);
    QTRY_VERIFY(terminal("styled-lora"));
    QCOMPARE(messages.last().value("code").toString(), QString("invalid-request"));
    for (const QJsonValue style : {QJsonValue(42), QJsonValue("cinematic")}) {
        auto invalidStyled = valid;
        invalidStyled.insert("styleId", style);
        if (style.isString()) invalidStyled.insert("prompt", " ");
        messages.clear(); send("invalid-style", invalidStyled);
        QTRY_VERIFY(terminal("invalid-style"));
        QCOMPARE(messages.last().value("code").toString(), QString("invalid-request"));
    }
    QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
    executable.write("#!/bin/sh\nprintf 'Klein fixture failure\\n' >&2\nexit 17\n");
    executable.close();
    messages.clear();
    auto failingOptions = valid;
    failingOptions.insert("prompt", "Klein's test");
    send("failure-command", failingOptions);
    QTRY_VERIFY(terminal("failure-command"));
    const auto failure = std::find_if(messages.cbegin(), messages.cend(), [](const QJsonObject& message) {
        return message.value("jobId") == "failure-command" && message.value("type") == "error";
    });
    QVERIFY(failure != messages.cend());
    const QString detail = failure->value("message").toString();
    QVERIFY(detail.contains("exit code 17"));
    QVERIFY(detail.contains("Command:"));
    QVERIFY(detail.contains("'--llm'"));
    QVERIFY(detail.contains(QDir(root).filePath(llm)));
    QVERIFY(detail.contains("'--vae'"));
    QVERIFY(detail.contains("Klein fixture failure"));
    QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
    executable.write("#!/bin/sh\nprintf '[WARN] Failed to allocate pinned memory: ErrorOutOfDeviceMemory\\n'\n"
        "previous=\"\"\nfor argument in \"$@\"; do\n"
        "if [ \"$previous\" = --output ]; then cp \"$KIRA_TEST_IMAGE\" \"$argument\"; fi\n"
        "previous=\"$argument\"\ndone\nexit 0\n");
    executable.close();
    messages.clear();
    send("pinned-memory-fallback", valid);
    QTRY_VERIFY(terminal("pinned-memory-fallback"));
    QCOMPARE(messages.last().value("type").toString(), QString("result"));
    QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
    executable.write("#!/bin/sh\nprintf '[ERROR] Vulkan allocation failed: ErrorOutOfDeviceMemory\\n'\nexec sleep 60\n");
    executable.close();
    messages.clear();
    send("memory-failure", valid);
    QTRY_VERIFY_WITH_TIMEOUT(terminal("memory-failure"), 5000);
    QCOMPARE(messages.last().value("type").toString(), QString("error"));
    QVERIFY(messages.last().value("message").toString().contains("ErrorOutOfDeviceMemory"));

    QVERIFY(detail.contains("Klein'\\''s test"));

}

void ServiceTest::validatesRecommendedOptions()
{
    using namespace KiraService::Protocol;
    QTemporaryDir directory;
    QVERIFY(QDir().mkpath(directory.filePath("inpaint")));
    QFile weights(directory.filePath("inpaint/test.gguf"));
    QVERIFY(weights.open(QIODevice::WriteOnly));
    weights.write("weights");
    weights.close();
    ModelCatalog catalog(directory.path(), false);
    const QString id = "inpaint:test.gguf";
    QVERIFY(catalog.modelById(id));
    QVERIFY(catalog.models(ModelCategory::ImageGeneration).isEmpty());
    auto inpaint = defaultModelOptions("inpaint");
    inpaint.insert("steps", 35);
    QVERIFY(catalog.configure(id, inpaint));
    ModelCatalog restarted(directory.path(), false);
    QCOMPARE(restarted.models(ModelCategory::Inpaint).size(), 1);
    QCOMPARE(restarted.modelById(id)->recommendedOptions, inpaint);
    QCOMPARE(defaultModelOptions("inpaint"), defaultModelOptions("image-generation"));
    QCOMPARE(ModelCatalog::categoryFromName("inpaint"), std::optional<ModelCategory>(ModelCategory::Inpaint));
    QCOMPARE(ModelCatalog::companionRole(ModelCategory::Inpaint), QString("vae"));
    for (auto category : {"image-generation", "inpaint", "edition", "llm-chat", "upscaler", "ip-adapter", "ip_adapter", "clip_vision", "photomaker", "prompt-enhancer"}) {
        auto options = defaultModelOptions(category); QVERIFY(validateModelOptions(category, options));
        options.insert("unrelated", true); QVERIFY(!validateModelOptions(category, options));
    }
    auto image = defaultModelOptions("image-generation"); image.insert("steps", "28"); QVERIFY(!validateModelOptions("image-generation", image));
    for (const auto& subtype : {"T5", "LLM", "VLM"}) {
        auto enhancer = defaultPromptEnhancerOptions(subtype);
        QVERIFY(validateModelOptions("prompt-enhancer", enhancer));
        enhancer.insert("maxOutputTokens", 0);
        QVERIFY(!validateModelOptions("prompt-enhancer", enhancer));
        enhancer = defaultPromptEnhancerOptions(subtype);
        enhancer.insert("doSample", "false");
        QVERIFY(!validateModelOptions("prompt-enhancer", enhancer));
    }
    QVERIFY(defaultPromptEnhancerOptions("unknown").isEmpty());
    auto t5 = defaultPromptEnhancerOptions("T5", true);
    QVERIFY(validateModelOptions("prompt-enhancer", t5));
    t5.insert("doSample", false);
    QVERIFY(!validateModelOptions("prompt-enhancer", t5));
    image.insert("steps", 28.5); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("steps", 200); image.insert("width", 2048); image.insert("height", 2048); QVERIFY(validateModelOptions("image-generation", image));
    image.insert("clipSkip", 0); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("clipSkip", 12); image.insert("sampler", "unknown"); QVERIFY(!validateModelOptions("image-generation", image));
    auto llm = defaultModelOptions("llm-chat"); llm.insert("maxTokens", 4097); QVERIFY(!validateModelOptions("llm-chat", llm));
    llm.insert("maxTokens", 4096); llm.insert("topP", 0); QVERIFY(!validateModelOptions("llm-chat", llm));
    QVERIFY(!validateModelOptions("upscaler", {{"tileSize", 129}}));
    QVERIFY(!validateModelOptions("ip-adapter", {{"strength", 2.1}}));
    QCOMPARE(defaultModelOptions("clip_vision"), QJsonObject({{"image_size", 224}, {"normalize_input", true}}));
    QCOMPARE(defaultModelOptions("ip_adapter"), QJsonObject({{"strength", 1.0}}));
    QVERIFY(validateModelOptions("ip_adapter", {{"strength", 0.8}}));
    auto vision = defaultModelOptions("clip_vision");
    for (const auto &value : {QJsonValue(0), QJsonValue(224.5), QJsonValue("224"), QJsonValue(4097)}) {
        vision.insert("image_size", value); QVERIFY(!validateModelOptions("clip_vision", vision));
    }
    vision = defaultModelOptions("clip_vision"); vision.insert("normalize_input", 1);
    QVERIFY(!validateModelOptions("clip_vision", vision));
    QCOMPARE(defaultModelOptions("photomaker"), QJsonObject({{"style_strength", 20}}));
    for (const auto &value : {QJsonValue(-1), QJsonValue(101), QJsonValue(20.5), QJsonValue("20"), QJsonValue(true)}) {
        QVERIFY(!validateModelOptions("photomaker", {{"style_strength", value}}));
    }
    QVERIFY(validateModelOptions("photomaker", {{"style_strength", 0}}));
    QVERIFY(validateModelOptions("photomaker", {{"style_strength", 100}}));
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
    QVERIFY(!catalog.modelById(id)->explicitContent);
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation"); QString error;
    QVERIFY2(catalog.configure(id, options, &error, true), qPrintable(error));
    QVERIFY(catalog.modelById(id)->explicitContent);
    QVERIFY(catalog.configure(id, options, &error));
    QVERIFY(catalog.modelById(id)->explicitContent);
    ModelCatalog restored(directory.path(), true); QCOMPARE(restored.modelById(id)->recommendedOptions, options);
    QVERIFY(restored.modelById(id)->explicitContent);
    QVERIFY(restored.configure(id, options, &error, false));
    QVERIFY(!catalog.modelById(id)->explicitContent);
    QCOMPARE(restored.modelById(id)->companionFileName, QString("companions/legacy.gguf/vae.gguf"));
    QVERIFY(file.open(QIODevice::ReadOnly)); auto metadata = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    QVERIFY(metadata.value("untouched.gguf").isString());
    auto bad = options; bad.insert("steps", -1); QVERIFY(!catalog.configure(id, bad, &error)); QCOMPARE(catalog.modelById(id)->recommendedOptions, options);
    QVERIFY(restored.removeCompanion(id, "vae", &error));
    QVERIFY(restored.modelById(id));
    QVERIFY(restored.modelById(id)->companionFileName.isEmpty());
    QCOMPARE(restored.modelById(id)->recommendedOptions, options);
    QVERIFY(!QFileInfo::exists(directory.filePath("companions/legacy.gguf/vae.gguf")));
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
    send({{"type", "models.configure"}, {"id", "image-generation:test.gguf"}, {"recommendedOptions", options}, {"explicitContent", true}}); QTRY_COMPARE(received.count(), 1);
    auto response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object(); QCOMPARE(response.value("type").toString(), QString("models.configure.completed")); QCOMPARE(response.value("model").toObject().value("recommendedOptions").toObject(), options); QVERIFY(!response.value("model").toObject().contains("filePath"));
    QVERIFY(response.value("model").toObject().value("explicitContent").toBool());
    send({{"type", "models.list"}}); QTRY_COMPARE(received.count(), 1); response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object();
    QCOMPARE(response.value("categories").toArray().first().toObject().value("models").toArray().first().toObject().value("recommendedOptions").toObject(), options);
    QVERIFY(response.value("categories").toArray().first().toObject().value("models").toArray().first().toObject().value("explicitContent").toBool());
    send({{"type", "models.configure"}, {"id", "image-generation:test.gguf"}, {"recommendedOptions", options}, {"explicitContent", "true"}});
    QTRY_COMPARE(received.count(), 1);
    response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object();
    QCOMPARE(response.value("code").toString(), QString("invalid-model-configure"));
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
    ModelInstallRequest request{QStringLiteral("install-1"), ModelCategory::Upscaler, server.url(), {}};
    request.explicitContent = true;
    QVERIFY(catalog.install(request, &error));
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(catalog.modelById("upscaler:model.gguf")->explicitContent);
    ModelCatalog restored(directory.path(), true);
    QVERIFY(restored.modelById("upscaler:model.gguf")->explicitContent);
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
        ModelInstallRequest request{fileName, ModelCategory::Edition, server.url(fileName), {}};
        request.companionUrls = {{"llm", server.url("encoder.gguf").toString()},
            {"mmproj", server.url("mmproj.gguf").toString()}};
        QVERIFY(catalog.install(request, &error));
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
        QCOMPARE(restored.modelById(id)->companionFiles.size(), 2);
        const QString encoderPath = directory.filePath("edition/" + model->companionFiles.value("llm").toString());
        const QString mmprojPath = directory.filePath("edition/" + model->companionFiles.value("mmproj").toString());
        QVERIFY(QFileInfo::exists(encoderPath));
        QVERIFY(QFileInfo::exists(mmprojPath));
        QVERIFY(catalog.remove(id, &error));
        QVERIFY(!QFileInfo::exists(encoderPath));
        QVERIFY(!QFileInfo::exists(mmprojPath));
        QVERIFY(!QFileInfo::exists(path));
    }
}

void ServiceTest::installsClipVision()
{
    TestHttpServer server("vision weights"); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    QString error;
    ModelInstallRequest request;
    request.installId = "vision"; request.category = ModelCategory::ClipVision;
    request.url = server.url("clip_vision_h.safetensors");
    request.recommendedOptions = KiraService::Protocol::defaultModelOptions("clip_vision");
    QVERIFY2(catalog.install(request, &error), qPrintable(error)); QTRY_COMPARE(finished.count(), 1);
    const auto id = QString("clip_vision:clip_vision_h.safetensors");
    const auto model = catalog.modelById(id); QVERIFY(model);
    QCOMPARE(model->filePath, directory.filePath("clip_vision/clip_vision_h.safetensors"));
    QCOMPARE(model->recommendedOptions, *request.recommendedOptions);
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(finished.count(), 2);
    QVERIFY(catalog.modelById("clip_vision:clip_vision_h-2.safetensors"));
    QVERIFY(catalog.configure(id, {{"image_size", 336}, {"normalize_input", false}}, &error));
    ModelCatalog restored(directory.path(), true);
    QCOMPARE(restored.modelById(id)->recommendedOptions, QJsonObject({{"image_size", 336}, {"normalize_input", false}}));
    QVERIFY(!catalog.configure(id, {{"image_size", 224}, {"normalize_input", "false"}}, &error));
    QVERIFY(catalog.remove(id, &error)); QVERIFY(!QFileInfo::exists(model->filePath));
    request.companionUrl = server.url("auxiliary.gguf");
    QVERIFY(!catalog.install(request, &error)); // CLIP Vision has only its required primary.
    QCOMPARE(ModelCatalog::categoryName(ModelCategory::IpAdapter), QString("ip_adapter"));
    QCOMPARE(*ModelCatalog::categoryFromName("ip_adapter"), ModelCategory::IpAdapter);
}

void ServiceTest::enhancesPromptThroughSdk()
{
    using namespace KiraService::Protocol;
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath("prompt-enhancer"));
    for (const auto& name : {"llm.gguf", "t5.gguf", "vlm.gguf", "wrong.safetensors"}) {
        QFile model(directory.filePath(QString("prompt-enhancer/%1").arg(name)));
        QVERIFY(model.open(QIODevice::WriteOnly)); model.write("fixture");
    }
    ModelCatalog catalog(directory.path(), false);
    auto parameters = defaultPromptEnhancerOptions("LLM");
    parameters.insert("systemPrompt", "Keep the description in French.");
    QVERIFY(catalog.configure("prompt-enhancer:llm.gguf", parameters));
    QVERIFY(catalog.configure("prompt-enhancer:t5.gguf", defaultPromptEnhancerOptions("T5")));
    QVERIFY(catalog.configure("prompt-enhancer:vlm.gguf", defaultPromptEnhancerOptions("VLM")));
    const auto executablePath = directory.filePath("completion");
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write(
        "#!/bin/sh\nroot=$(dirname \"$0\")\nprintf '%s\\n' \"$@\" > \"$root/args\"\n"
        "previous=\nfor argument in \"$@\"; do\n"
        "if [ \"$previous\" = --file ]; then cat \"$argument\" > \"$root/input\"; fi\n"
        "if [ \"$previous\" = --system-prompt-file ]; then cat \"$argument\" > \"$root/system\"; fi\n"
        "if [ \"$previous\" = --repeat-last-n ] && [ \"$argument\" -lt 0 ]; then exit 1; fi\n"
        "previous=\"$argument\"\ndone\n"
        "if [ -f \"$root/wait\" ]; then exec sleep 30; fi\n"
        "if [ -f \"$root/fail\" ]; then printf 'backend failure' >&2; exit 9; fi\n"
        "if [ -f \"$root/empty\" ]; then exit 0; fi\n"
        "if [ -f \"$root/large\" ]; then head -c 1048577 /dev/zero; exit 0; fi\n"
        "printf 'warning on stderr\\n' >&2\nprintf '  portrait détaillé\\n [end of text]\\n'\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    configuration.llamaCompletion = executablePath;
    configuration.promptEnhancementTimeoutMs = 400;
    KiraServiceDaemon daemon(configuration); QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    QSignalSpy finished(&client, &KiraService::ServiceClient::promptEnhancementFinished);
    QSignalSpy imageFinished(&client, &KiraService::ServiceClient::imageJobFinished);
    client.connectToService();
    QTRY_VERIFY(client.canEnhancePrompt("prompt-enhancer:llm.gguf"));
    QVERIFY(client.enhancePrompt({"missing", "person", {}}).isEmpty());
    QVERIFY(!client.enhancePrompt({"prompt-enhancer:llm.gguf", "  ", {}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
    QFile emptyInput(directory.filePath("input"));
    QVERIFY(emptyInput.open(QIODevice::ReadOnly));
    QVERIFY(emptyInput.readAll().contains("Generate a new description"));
    const QString prompt = "une personne\navec des cheveux bouclés; $(false)";
    auto id = client.enhancePrompt({"prompt-enhancer:llm.gguf", prompt, {{"maxOutputTokens", 80}, {"doSample", false}}});
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    auto result = qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0));
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QCOMPARE(result.jobId, id); QCOMPARE(result.prompt, QString("portrait détaillé"));
    QCOMPARE(imageFinished.count(), 0);
    const auto read = [&](const QString& name) {
        QFile file(directory.filePath(name)); if (!file.open(QIODevice::ReadOnly)) return QByteArray(); return file.readAll();
    };
    const auto arguments = read("args");
    QVERIFY(arguments.contains("--ctx-size\n4096\n"));
    QVERIFY(arguments.contains("--repeat-last-n\n4096\n"));
    QVERIFY(arguments.contains("--n-predict\n80\n"));
    QVERIFY(arguments.contains("--temp\n0\n"));
    QVERIFY(arguments.contains("--top-k\n20\n"));
    QVERIFY(arguments.contains("--single-turn\n"));
    QVERIFY(arguments.contains("--no-display-prompt\n"));
    QCOMPARE(QString::fromUtf8(read("input")), prompt);
    QVERIFY(read("system").contains("Keep the description in French."));
    QCOMPARE(read("system"), QByteArray("Keep the description in French."));
    const auto expectFailure = [&](const KiraService::PromptEnhancementRequest& request, const QString& code) {
        QVERIFY(!client.enhancePrompt(request).isEmpty());
        QTRY_COMPARE(finished.count(), 1);
        const auto failure = qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0));
        QVERIFY(!failure.succeeded); QCOMPARE(failure.errorCode, code); QVERIFY(failure.prompt.isEmpty());
    };
    expectFailure({"prompt-enhancer:t5.gguf", "person", {}}, "prompt-enhancer-subtype-unsupported");
    expectFailure({"prompt-enhancer:vlm.gguf", "person", {}}, "prompt-enhancer-subtype-unsupported");
    expectFailure({"prompt-enhancer:wrong.safetensors", "person", {}}, "prompt-enhancer-format-unsupported");
    expectFailure({"prompt-enhancer:llm.gguf", "person", {{"subtype", "T5"}}}, "invalid-request");
    expectFailure({"prompt-enhancer:llm.gguf", "person", {{"temperature", "hot"}}}, "invalid-request");
    const auto marker = [&](const QString& name) { QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly); };
    QVERIFY(marker("fail"));
    expectFailure({"prompt-enhancer:llm.gguf", "person", {}}, "prompt-enhancement-failed");
    QVERIFY(QFile::remove(directory.filePath("fail")));
    QVERIFY(marker("empty"));
    expectFailure({"prompt-enhancer:llm.gguf", "person", {}}, "prompt-enhancement-empty-result");
    QVERIFY(QFile::remove(directory.filePath("empty")));
    QVERIFY(marker("large"));
    expectFailure({"prompt-enhancer:llm.gguf", "person", {}}, "prompt-enhancement-output-too-large");
    QVERIFY(QFile::remove(directory.filePath("large")));
    QVERIFY(marker("wait"));
    expectFailure({"prompt-enhancer:llm.gguf", "person", {}}, "prompt-enhancement-timeout");
    QVERIFY(QFile::remove(directory.filePath("wait")));
    QVERIFY(!client.enhancePrompt({"prompt-enhancer:llm.gguf", "retry", {}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
    // Disconnect cancels active and queued work and releases the shared queue.
    QVERIFY(marker("wait"));
    QVERIFY(QFile::remove(directory.filePath("args")));
    QVERIFY(!client.enhancePrompt({"prompt-enhancer:llm.gguf", "active", {}}).isEmpty());
    QVERIFY(!client.enhancePrompt({"prompt-enhancer:llm.gguf", "queued", {}}).isEmpty());
    QTRY_VERIFY(QFileInfo::exists(directory.filePath("args")));
    client.disconnectFromService();
    QTRY_COMPARE(finished.count(), 2);
    for (const auto& notification : finished) {
        QVERIFY(!qvariant_cast<KiraService::PromptEnhancementResult>(notification.at(0)).succeeded);
    }
    finished.clear();
    QVERIFY(QFile::remove(directory.filePath("wait")));
    client.connectToService();
    QTRY_VERIFY(client.canEnhancePrompt("prompt-enhancer:llm.gguf"));
    QVERIFY(!client.enhancePrompt({"prompt-enhancer:llm.gguf", "after reconnect", {}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
}

void ServiceTest::installsAndPersistsPromptEnhancer()
{
    TestHttpServer server("enhancer weights"); QVERIFY(server.start());
    QTemporaryDir directory;
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    ModelInstallRequest request;
    request.installId = "enhancer"; request.category = ModelCategory::PromptEnhancer;
    request.url = server.url("enhancer.gguf");
    request.recommendedOptions = KiraService::Protocol::defaultPromptEnhancerOptions("LLM");
    QString error;
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(finished.count(), 1);
    const auto installed = catalog.models(ModelCategory::PromptEnhancer);
    QCOMPARE(installed.size(), 1);
    const auto id = installed.first().id;
    QCOMPARE(installed.first().filePath, directory.filePath("prompt-enhancer/enhancer.gguf"));
    const auto t5 = KiraService::Protocol::defaultPromptEnhancerOptions("T5");
    QVERIFY(catalog.configure(id, t5, &error, true));
    ModelCatalog restored(directory.path(), true);
    QCOMPARE(restored.modelById(id)->recommendedOptions, t5);
    QVERIFY(restored.modelById(id)->explicitContent);
    request.companionUrl = server.url("vision.gguf");
    QVERIFY(!catalog.install(request, &error));
    QVERIFY(catalog.remove(id, &error));
    QVERIFY(restored.models(ModelCategory::PromptEnhancer).isEmpty());
}

void ServiceTest::installsPhotoMaker()
{
    TestHttpServer server("photomaker weights"); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    QString error;
    ModelInstallRequest request;
    request.installId = "photomaker"; request.category = ModelCategory::PhotoMaker;
    request.url = server.url("photomaker-v2.safetensors");
    request.recommendedOptions = KiraService::Protocol::defaultModelOptions("photomaker");
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(finished.count(), 1);
    const QString id = "photomaker:photomaker-v2.safetensors";
    const auto model = catalog.modelById(id); QVERIFY(model);
    QCOMPARE(model->filePath, directory.filePath("photomaker/photomaker-v2.safetensors"));
    QCOMPARE(model->recommendedOptions, *request.recommendedOptions);
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(finished.count(), 2);
    QVERIFY(catalog.modelById("photomaker:photomaker-v2-2.safetensors"));
    QVERIFY(catalog.configure(id, {{"style_strength", 100}}, &error));
    ModelCatalog restored(directory.path(), true);
    QCOMPARE(restored.modelById(id)->recommendedOptions, QJsonObject({{"style_strength", 100}}));
    QVERIFY(!catalog.configure(id, {{"style_strength", 20.5}}, &error));
    QVERIFY(catalog.remove(id, &error));
    request.url = server.url("photomaker.bin"); QVERIFY(!catalog.install(request, &error));
    request.url = server.url("photomaker.safetensors");
    request.companionUrl = server.url("auxiliary.gguf"); QVERIFY(!catalog.install(request, &error));
}

void ServiceTest::exportsPhotoMakerIdentity()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("python3 is required for the identity-extractor fixture");
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    QVERIFY(QDir().mkpath(fixture.filePath("insightface")));
    const auto write = [&](const QString &name, const QByteArray &data) {
        QFile file(fixture.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    };
    QVERIFY(write("insightface/__init__.py", ""));
    QVERIFY(write("insightface/app.py",
        "class FaceAnalysis:\n"
        "    def get(self, image, max_num=0):\n"
        "        if image == b'retry':\n"
        "            self.attempts = getattr(self, 'attempts', 0) + 1\n"
        "            return [] if self.attempts == 1 else [{}]\n"
        "        return [{}] * (2 if image == b'multiple' else 0 if image == b'none' or image.endswith(b'missingface') else 1)\n"));
    // Exercise the bundled guard around the official subclass/get and runpy entry point.
    // Replace heavyweight inference only; no model download or GPU is used in this test.
    QVERIFY(write("face_detect.py", R"PY(
import pathlib, sys, time
from insightface.app import FaceAnalysis
class FaceAnalysis2(FaceAnalysis):
    def get(self, img, max_num=0, det_size=(640, 640)):
        return super().get(img, max_num)
directory = pathlib.Path(sys.argv[1])
pathlib.Path(__file__ + ".log").write_text(str(directory))
names = sorted(path.name for path in directory.iterdir())
assert names == ["input.png"] or names == ["input.png", "reference-2.png", "reference-3.png", "reference-4.png"]
if len(names) == 4:
    for name in names:
        assert (directory / name).read_bytes().startswith(bytes.fromhex("89504e470d0a1a0a"))
image = (directory / "input.png").read_bytes()
if image == b"failure":
    raise RuntimeError("fixture extraction failure")
if image == b"missing":
    sys.exit(0)
if image == b"slow":
    time.sleep(60)
detector = FaceAnalysis2()
faces = detector.get(image)
if image == b"retry" and not faces:
    faces = detector.get(image)
for name in names[1:]:
    detector.get((directory / name).read_bytes())
if not faces:
    raise ValueError("No face detected in input image pool")
(directory / "id_embeds.bin").write_bytes(b"\x04\x00\x00\x00identity-fixture")
)PY"));
    const QByteArray oldPythonPath = qgetenv("PYTHONPATH");
    const auto restore = qScopeGuard([oldPythonPath] {
        if (oldPythonPath.isNull()) qunsetenv("PYTHONPATH");
        else qputenv("PYTHONPATH", oldPythonPath);
    });
    qputenv("PYTHONPATH", fixture.path().toUtf8());
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = fixture.filePath("models");
    configuration.photoMakerPython = python;
    configuration.photoMakerFaceDetect = fixture.filePath("face_detect.py");
    KiraServiceDaemon service(configuration);
    QString error; QVERIFY2(service.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    QSignalSpy results(&client, &KiraService::ServiceClient::photoMakerIdentityExportFinished);
    QSignalSpy images(&client, &KiraService::ServiceClient::imageJobFinished);
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())), {});
    client.connectToService();
    QTRY_VERIFY(client.capabilities().photoMakerIdentityExport);
    QVERIFY(client.exportPhotoMakerIdentity({}).isEmpty());
    const auto png = QByteArray::fromHex("89504e470d0a1a0a") + "fixture";
    QVERIFY(!client.exportPhotoMakerIdentityImages({png, png + "2", png + "3", png + "4"}).isEmpty());
    QTRY_COMPARE(results.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PhotoMakerIdentityResult>(results.first().first()).succeeded);
    results.clear();
    QVERIFY(!client.exportPhotoMakerIdentityImages({png, png + "missingface", png + "3", png + "4"}).isEmpty());
    QTRY_COMPARE(results.count(), 1);
    QCOMPARE(qvariant_cast<KiraService::PhotoMakerIdentityResult>(results.first().first()).errorCode, QString("photomaker-no-face"));
    for (const auto &source : {QByteArray("single"), QByteArray("retry"), QByteArray("none"), QByteArray("multiple"),
                              QByteArray("failure"), QByteArray("missing")}) {
        results.clear();
        const QString id = client.exportPhotoMakerIdentity(source); QVERIFY(!id.isEmpty());
        QTRY_COMPARE(results.count(), 1);
        const auto result = qvariant_cast<KiraService::PhotoMakerIdentityResult>(results.first().first());
        QCOMPARE(result.jobId, id);
        if (source == "single" || source == "retry") {
            QVERIFY2(result.succeeded, qPrintable(result.message));
            QCOMPARE(result.identityData, QByteArray::fromHex("04000000") + "identity-fixture");
        } else {
            QVERIFY(!result.succeeded); QVERIFY(result.identityData.isEmpty());
            const QString expected = source == "none" ? "photomaker-no-face"
                : source == "multiple" ? "photomaker-multiple-faces"
                : source == "missing" ? "photomaker-output-missing" : "photomaker-extraction-failed";
            QCOMPARE(result.errorCode, expected); QVERIFY(!result.message.isEmpty());
        }
        QFile log(fixture.filePath("face_detect.py.log")); QVERIFY(log.open(QIODevice::ReadOnly));
        const QString workingDirectory = QString::fromUtf8(log.readAll());
        QTRY_VERIFY(!QFileInfo::exists(workingDirectory));
        QCOMPARE(images.count(), 0);
    }
    results.clear();
    const QString canceled = client.exportPhotoMakerIdentity("slow"); QVERIFY(!canceled.isEmpty());
    QFile log(fixture.filePath("face_detect.py.log"));
    QString workingDirectory;
    QTRY_VERIFY(([&] {
        if (!log.open(QIODevice::ReadOnly)) return false;
        workingDirectory = QString::fromUtf8(log.readAll()); log.close();
        return QFileInfo::exists(workingDirectory);
    })());
    client.disconnectFromService();
    QTRY_COMPARE(results.count(), 1);
    QCOMPARE(qvariant_cast<KiraService::PhotoMakerIdentityResult>(results.first().first()).errorCode,
        QString("connection-closed"));
    QTRY_VERIFY(!QFileInfo::exists(workingDirectory));
    client.connectToService(); QTRY_VERIFY(client.capabilities().photoMakerIdentityExport);
    results.clear(); QVERIFY(!client.exportPhotoMakerIdentity("single").isEmpty());
    QTRY_COMPARE(results.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PhotoMakerIdentityResult>(results.first().first()).succeeded);
    QCOMPARE(images.count(), 0);
}

void ServiceTest::installsIpAdapterPair()
{
    TestHttpServer server(QByteArray("model-bytes")); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished); QString error;
    ModelInstallRequest request{QStringLiteral("ip-adapter-install"), ModelCategory::IpAdapter,
                                      server.url(QStringLiteral("ip-adapter-faceid-plusv2_sdxl.bin")),
                                      server.url(QStringLiteral("ip-adapter-faceid-plusv2_sdxl_lora.safetensors"))};
    request.recommendedOptions = KiraService::Protocol::defaultModelOptions("ip_adapter");
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
    QVERIFY(models.first().id.startsWith("ip-adapter:"));
    QVERIFY(catalog.model(ModelCategory::IpAdapter, "ip_adapter:" + models.first().fileName));
    QVERIFY(catalog.configure(models.first().id, {{"strength", 0.75}}, &error));
    ModelCatalog restoredCatalog(directory.path(), true);
    QCOMPARE(restoredCatalog.models(ModelCategory::IpAdapter).first().companionFileName, companionRelativePath);
    QCOMPARE(restoredCatalog.models(ModelCategory::IpAdapter).first().recommendedOptions,
        QJsonObject({{"strength", 0.75}}));
    QVERIFY(catalog.removeCompanion(models.first().id, "lora", &error));
    QVERIFY(QFileInfo::exists(primaryPath)); QVERIFY(!QFileInfo::exists(companionPath));
    ModelInstallRequest attachment;
    attachment.installId = "adapter-auxiliary"; attachment.category = ModelCategory::IpAdapter;
    attachment.existingModelId = models.first().id; attachment.modelType.clear();
    attachment.companionUrls = {{"lora", server.url("replacement.safetensors").toString()}};
    QVERIFY2(catalog.install(attachment, &error), qPrintable(error)); QTRY_COMPARE(finished.count(), 2);
    const auto updated = catalog.modelById(models.first().id); QVERIFY(updated);
    QCOMPARE(updated->companionFiles.value("lora").toString(),
        QString("companions/%1/lora/replacement.safetensors").arg(updated->fileName));
    QCOMPARE(updated->recommendedOptions, QJsonObject({{"strength", 0.75}}));
    QVERIFY(!catalog.install(attachment, &error));
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
        {"vae", server.url("vae.safetensors").toString()},
        {"llm", server.url("encoder.gguf").toString()}, {"mmproj", server.url("mmproj.gguf").toString()}};
    QString error;
    QSignalSpy done(&catalog, &ModelCatalog::installFinished);
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(done.count(), 1);
    ModelCatalog restored(directory.path(), true);
    const auto model = restored.modelById(request.existingModelId);
    QVERIFY(model);
    QCOMPARE(model->modelType, QString("diffusion"));
    QCOMPARE(model->companionFiles.size(), 5);
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

void ServiceTest::importsLocalModelWithoutChangingSources()
{
    QTemporaryDir source;
    QTemporaryDir storage;
    const QByteArray bytes("local model bytes");
    for (const auto& name : {"local model.gguf", "encoder.gguf", "mmproj.gguf"}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
    }
    ModelCatalog catalog(storage.path(), true);
    ModelInstallRequest request;
    request.installId = "local-import";
    request.url = QUrl::fromLocalFile(source.filePath("local model.gguf"));
    request.companionUrls = {{"llm", QUrl::fromLocalFile(source.filePath("encoder.gguf")).toString()},
        {"mmproj", QUrl::fromLocalFile(source.filePath("mmproj.gguf")).toString()}};
    QString error;
    QVERIFY(!catalog.install(request, &error));
    request.allowLocalFiles = true;
    QSignalSpy completed(&catalog, &ModelCatalog::installFinished);
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(completed.count(), 1);
    const QString id = "image-generation:local model.gguf";
    ModelCatalog restored(storage.path(), true);
    const auto model = restored.modelById(id);
    QVERIFY(model);
    QCOMPARE(model->companionFiles.size(), 2);
    QFile installed(model->filePath);
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), bytes);
    installed.close();
    QVERIFY(!catalog.removeCompanion(id, "unknown", &error));
    QVERIFY(catalog.removeCompanion(id, "llm", &error));
    QVERIFY(QFileInfo::exists(model->filePath));
    QVERIFY(!QFileInfo::exists(storage.filePath(model->companionFiles.value("llm").toString())));
    QCOMPARE(catalog.modelById(id)->companionFiles.size(), 1);
    QCOMPARE(catalog.modelById(id)->companionFiles.value("mmproj"), model->companionFiles.value("mmproj"));
    ModelInstallRequest replacement;
    replacement.installId = "replacement";
    replacement.existingModelId = id;
    replacement.allowLocalFiles = true;
    replacement.companionUrls = {{"llm", QUrl::fromLocalFile(source.filePath("encoder.gguf")).toString()}};
    QVERIFY2(catalog.install(replacement, &error), qPrintable(error));
    QTRY_COMPARE(completed.count(), 2);
    QCOMPARE(catalog.modelById(id)->companionFiles.size(), 2);
    QVERIFY(QFileInfo::exists(storage.filePath(model->companionFiles.value("llm").toString())));
    request.installId = "duplicate-import";
    request.displayName = "  My second model  ";
    QVERIFY2(catalog.install(request, &error), qPrintable(error));
    QTRY_COMPARE(completed.count(), 3);
    const auto duplicate = catalog.modelById("image-generation:local model-2.gguf");
    QVERIFY(duplicate);
    QCOMPARE(duplicate->name, QString("My second model"));
    QCOMPARE(duplicate->displayName, QString("My second model"));
    QVERIFY(duplicate->companionFiles.value("llm").toString().contains("local model-2.gguf/"));
    QCOMPARE(catalog.modelById(id)->fileName, QString("local model.gguf"));
    ModelCatalog restoredNames(storage.path(), true);
    QCOMPARE(restoredNames.modelById(duplicate->id)->displayName, duplicate->displayName);
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    QVERIFY(catalog.configure(duplicate->id, options, &error, {}, QString()));
    QCOMPARE(catalog.modelById(duplicate->id)->name, QString("local model-2.gguf"));
    QVERIFY(catalog.configure(duplicate->id, options, &error, {}, QString("Renamed model")));
    QCOMPARE(catalog.modelById(duplicate->id)->name, QString("Renamed model"));
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = storage.path();
    KiraServiceDaemon service(configuration);
    QVERIFY2(service.start(&error), qPrintable(error));
    QWebSocket socket;
    QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
    socket.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())));
    QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
    socket.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}");
    QTRY_VERIFY(messages.count() >= 2);
    messages.clear();
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "models.companion.delete"}, {"id", id}, {"role", "mmproj"}}).toJson()));
    QTRY_COMPARE(messages.count(), 1);
    const auto response = QJsonDocument::fromJson(messages.first().first().toString().toUtf8()).object();
    QCOMPARE(response.value("type").toString(), QString("models.companion.delete.completed"));
    QCOMPARE(response.value("id").toString(), id);
    QCOMPARE(response.value("role").toString(), QString("mmproj"));
    QVERIFY(!catalog.modelById(id)->companionFiles.contains("mmproj"));
    QVERIFY(catalog.modelById(id)->companionFiles.contains("llm"));
    QVERIFY(catalog.remove(id, &error));
    for (const auto& name : {"local model.gguf", "encoder.gguf", "mmproj.gguf"}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
}
