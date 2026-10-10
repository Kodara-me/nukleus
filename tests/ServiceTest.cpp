#include "KiraService.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include "SdCliProgressParser.h"
#include "SdCliRunner.h"
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

namespace {
bool installNamed(ModelCatalog& catalog, ModelInstallRequest request, QString* error = nullptr)
{
    if (request.existingModelId.isEmpty() && !request.displayName) request.displayName = "Fixture model";
    return catalog.install(request, error);
}
bool configureNamed(ModelCatalog& catalog, const QString& id, const QJsonObject& options,
    QString* error = nullptr, std::optional<bool> explicitContent = {}, std::optional<QString> displayName = {})
{
    if (!displayName) {
        const auto model = catalog.modelById(id);
        displayName = model ? model->name : QString("Fixture model");
    }
    return catalog.configure(id, options, error, explicitContent, displayName);
}
}

class ServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void persistsAndProtectsModelRouting();
    void fixedRoutingLabelsAndLegacyMigration();
    void configuresModelRoutingThroughSdk();
    void keepsSingleDefaultPromptLlms();
    void persistsPromptStyles();
    void enhancesPromptThroughSdk();
    void chatsThroughCompletionEngine();
    void chatsThroughServerEngine();
    void chatsWithImagesThroughVisionProjector();
    void routesPresetGenerationWithoutLlm();
    void configuresStylesOverProtocol();
    void generatesWithValidatedOptionsAndVae();
    void validatesRecommendedOptions();
    void reportsTheRunningSamplingStep();
    void readsProgressBeforeTheLineEnds();
    void assignsImageReferencesAndProtectsSharedModels();
    void persistsOptionsAndLegacyMetadata();
    void reservesSingleInstallAndCancels();
    void cancelsDuringCompanionDownload();
    void rollsBackMetadataFailure();
    void configuresModelsOverProtocol();
    void preservesProtocolV3();
    void rejectsRetiredCategoriesAndOperations();
    void requiresNamesAndAssignsSharedMmproj();
    void importsLocalModelWithoutChangingSources();
    void rejectsLanWithoutToken();
    void startsOnLoopbackAndLanAddress();
    void authenticatesLoopbackWithoutToken();
    void rejectsLanAuthenticationWithoutToken();
    void rollsBackWhenLanListenerFails();
    void listsSupportedModelsWithoutPathLeak();
    void removesModelByOpaqueId();
    void installsModelAtomically();
    void installsOptionalCompanions();
    void attachesModularCompanionsWithoutReplacingModel();
    void attachesFilesOverProtocol();
    void rollsBackFailedCompanion();
    void acceptsExternalModelUrls();
    void rejectsInvalidModelInstall();
    void migratesLegacyModelRoot();
    void rejectsConflictingModelRoots();
};

void ServiceTest::persistsAndProtectsModelRouting()
{
    QTemporaryDir directory;
    ModelCatalog catalog(directory.path(), false);
    QString error;
    auto routing = catalog.routing(&error);
    QVERIFY2(routing.has_value(), qPrintable(error));
    QCOMPARE(routing->presets.size(), 2);
    QCOMPARE(routing->presets.first().name, QString("Photorealism"));
    QVERIFY(routing->presets.first().model.modelId.isEmpty());
    QCOMPARE(routing->editingQualities.size(), 3);
    QCOMPARE(routing->defaultEditingQuality()->id, QString("edit-medium"));
    for (const auto &quality : routing->editingQualities) QVERIFY(quality.model.modelId.isEmpty());
    QVERIFY(routing->presets.first().positivePrefix.isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath("model-routing.json")));
    QFile model(directory.filePath("test.gguf"));
    QVERIFY(model.open(QIODevice::WriteOnly)); model.write("weights"); model.close();
    const QString id = "image-generation:test.gguf";
    auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    options.insert("steps", 37); options.insert("cfgScale", 5.75);
    QVERIFY2(catalog.configure(id, options, &error, {}, "Test"), qPrintable(error));
    routing->presets[0].model.modelId = id;
    routing->presets[1].model.modelId = id;
    routing->editingQualities[2].model.modelId = id;
    QVERIFY2(catalog.configureRouting(*routing, &error), qPrintable(error));
    routing = catalog.routing(&error);
    QCOMPARE(routing->presets[0].model.steps, std::optional<int>(37));
    QCOMPARE(routing->presets[0].model.cfgScale, std::optional<double>(5.75));
    QCOMPARE(routing->editingQualities[2].model.steps, std::optional<int>(37));
    QVERIFY(!routing->editingQualities[0].model.steps); // Unassigned qualities keep no snapshot.
    routing->presets[0].model.steps = 4;
    routing->presets[0].model.cfgScale = 1.0;
    QVERIFY(catalog.configureRouting(*routing, &error));
    QCOMPARE(catalog.routing(), routing);
    QVERIFY(!catalog.remove(id, &error));
    QVERIFY(!catalog.configure(id, {{"canGenerate", false}, {"canEdit", true}}, &error, {}, "Test"));
    QVERIFY(!catalog.configure(id, {{"canGenerate", true}, {"canEdit", false}}, &error, {}, "Test"));
    QVERIFY(QDir(directory.path()).mkpath("llm"));
    QFile llmFile(directory.filePath("llm/chat.gguf"));
    QVERIFY(llmFile.open(QIODevice::WriteOnly)); llmFile.write("weights"); llmFile.close();
    const QString llmId = "llm-chat:chat.gguf";
    const auto llmOptions = KiraService::Protocol::defaultModelOptions("llm-chat");
    QVERIFY2(catalog.configure(llmId, llmOptions, &error, {}, "Chat"), qPrintable(error));
    QVERIFY2(catalog.remove(llmId, &error), qPrintable(error)); // Routing never holds LLMs.
    auto invalid = *routing;
    invalid.presets[0].name = " ";
    QVERIFY(!catalog.configureRouting(invalid, &error));
    invalid = *routing; invalid.presets[0].model.modelId = "image-generation:missing.gguf";
    QVERIFY(!catalog.configureRouting(invalid, &error));
    invalid = *routing; invalid.editingQualities[0].model.modelId = "image-generation:missing.gguf";
    QVERIFY(!catalog.configureRouting(invalid, &error));
    QVERIFY(model.remove());
    QCOMPARE(catalog.routing(), routing); // External disappearance preserves the assignment.
    QVERIFY(!catalog.configureRouting({}, &error));
    QVERIFY(catalog.configureRouting(KiraService::Protocol::starterModelRouting(), &error));
    QCOMPARE(catalog.routing()->presets.size(), 2);
#ifdef Q_OS_UNIX
    const auto permissions = QFileInfo(directory.path()).permissions();
    const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(directory.path(), permissions); });
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
    QFile probe(directory.filePath("write-probe"));
    if (!probe.open(QIODevice::WriteOnly)) {
        QVERIFY(!catalog.configureRouting(KiraService::Protocol::starterModelRouting(), &error));
        QCOMPARE(catalog.routing(), std::optional<KiraService::ModelRouting>(KiraService::Protocol::starterModelRouting()));
    }
    probe.close();
    QVERIFY(QFile::setPermissions(directory.path(), permissions));
#endif
    QFile config(directory.filePath("model-routing.json"));
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate)); config.write("broken"); config.close();
    QVERIFY(!catalog.routing(&error));
    QVERIFY(!catalog.configureRouting({}, &error));
    QVERIFY(config.open(QIODevice::ReadOnly)); QCOMPARE(config.readAll(), QByteArray("broken")); config.close();
    QVERIFY(config.remove());
    QVERIFY(QDir().mkdir(config.fileName()));
    QVERIFY(!catalog.configureRouting({}, &error));
}

void ServiceTest::fixedRoutingLabelsAndLegacyMigration()
{
    const auto fixed = KiraService::Protocol::starterModelRouting();
    QCOMPARE(fixed.presets[0].displayLabel(QLocale("fr_CA")), QString("Photoréalisme"));
    QCOMPARE(fixed.presets[0].displayLabel(QLocale("en_US")), QString("Photorealism"));
    QCOMPARE(fixed.presets[1].displayLabel(QLocale("fr_FR")), QString("Illustration"));
    // Versions 2 and 3 stored Fast/Balanced/Quality tiers; only the Fast tier carries over.
    const auto tiers = [](const QString &prefix, const QString &model) {
        QJsonArray list;
        const QStringList levels{"low", "medium", "high"};
        for (int i = 0; i < levels.size(); ++i)
            list.append(QJsonObject{{"id", prefix + "-" + levels[i]}, {"name", levels[i]}, {"nameFr", levels[i]},
                                    {"modelId", model + levels[i]}, {"permanent", i == 1},
                                    {"steps", 4 + i}, {"cfgScale", 1.25 + i}});
        return list;
    };
    const QJsonObject tiered{{"version", 3},
        {"presets", QJsonArray{
            QJsonObject{{"id", "photorealism"}, {"name", "Photorealism"}, {"nameFr", "Photoréalisme"},
                        {"positivePrefix", "saved prefix"}, {"negativePrefix", "saved negative"},
                        {"qualities", tiers("photo", "image-generation:photo-")},
                        {"promptFlow", QJsonArray{QJsonObject{{"id", "enhance"}, {"name", "Enhance"},
                            {"nameFr", "Améliorer"}, {"modelId", "llm-chat:chat.gguf"}, {"instructions", "Improve."}}}}},
            QJsonObject{{"id", "illustration"}, {"name", "Illustration"}, {"nameFr", "Illustration"},
                        {"positivePrefix", ""}, {"negativePrefix", ""},
                        {"qualities", tiers("illustration", "image-generation:drawn-")}, {"promptFlow", QJsonArray{}}}}},
        {"editingQualities", tiers("edit", "image-generation:edit-")}};
    auto migrated = KiraService::Protocol::modelRoutingFromJson(tiered);
    QVERIFY(migrated.has_value());
    QCOMPARE(migrated->presets[0].positivePrefix, QString("saved prefix"));
    QCOMPARE(migrated->presets[0].negativePrefix, QString("saved negative"));
    QCOMPARE(migrated->presets[0].model, (KiraService::ModelAssignment{"image-generation:photo-low", 4, 1.25}));
    QCOMPARE(migrated->presets[1].model, (KiraService::ModelAssignment{"image-generation:drawn-low", 4, 1.25}));
    // Editing keeps all three tiers; the permanent Balanced tier becomes the default.
    QCOMPARE(migrated->editingQualities[0].model, (KiraService::ModelAssignment{"image-generation:edit-low", 4, 1.25}));
    QCOMPARE(migrated->editingQualities[1].model, (KiraService::ModelAssignment{"image-generation:edit-medium", 5, 2.25}));
    QCOMPARE(migrated->editingQualities[2].model, (KiraService::ModelAssignment{"image-generation:edit-high", 6, 3.25}));
    QCOMPARE(migrated->defaultEditingQuality()->id, QString("edit-medium"));
    QCOMPARE(migrated->editingQualities[0].name, QString("Fast"));
    auto version2 = tiered; version2.insert("version", 2); // Retired prompt flows are ignored.
    migrated = KiraService::Protocol::modelRoutingFromJson(version2);
    QVERIFY(migrated.has_value());
    QCOMPARE(migrated->presets[0].identityViews, 1);
    QCOMPARE(migrated->presets[0].model.modelId, QString("image-generation:photo-low"));
    QCOMPARE(KiraService::Protocol::modelRoutingFromJson(KiraService::Protocol::modelRoutingToJson(fixed)),
        std::optional<KiraService::ModelRouting>(fixed));
    const auto fixedJson = KiraService::Protocol::modelRoutingToJson(fixed);
    QCOMPARE(fixedJson.value("version").toInt(), 6);
    QVERIFY(!fixedJson.value("presets").toArray().first().toObject().contains("promptFlow"));
    auto legacyFlow = fixedJson; // Version-6 documents saved before the removal still load.
    auto legacyPresets = legacyFlow.value("presets").toArray();
    auto legacyPreset = legacyPresets[0].toObject();
    legacyPreset.insert("promptFlow", QJsonArray{QJsonObject{{"id", "enhance"}, {"name", "Enhance"}}});
    legacyPresets[0] = legacyPreset;
    legacyFlow.insert("presets", legacyPresets);
    QCOMPARE(KiraService::Protocol::modelRoutingFromJson(legacyFlow), std::optional<KiraService::ModelRouting>(fixed));
    auto unknownVersion = fixedJson; unknownVersion.insert("version", 7);
    QVERIFY(!KiraService::Protocol::modelRoutingFromJson(unknownVersion));
    auto invalid = fixed; invalid.presets.swapItemsAt(0, 1);
    QVERIFY(KiraService::Protocol::validateModelRouting(invalid)); // Presets may be reordered.
    invalid = fixed; invalid.presets.append({"portrait", "Portrait", {}, {}, {}, "Portrait"});
    QVERIFY(KiraService::Protocol::validateModelRouting(invalid)); // Presets may be added...
    QCOMPARE(KiraService::Protocol::modelRoutingFromJson(KiraService::Protocol::modelRoutingToJson(invalid)),
        std::optional<KiraService::ModelRouting>(invalid));
    invalid.presets.removeFirst();
    invalid.presets.removeFirst();
    QVERIFY(KiraService::Protocol::validateModelRouting(invalid)); // ...and removed, keeping at least one.
    invalid.presets.clear();
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.presets[1].id = invalid.presets[0].id;
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid)); // Duplicate preset ID.
    invalid = fixed; invalid.presets[0].nameFr = " ";
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.presets[0].name = QString(257, 'x');
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed;
    while (invalid.presets.size() < KiraService::Protocol::MaximumGenerationPresets)
        invalid.presets.append({QString("extra-%1").arg(invalid.presets.size()), "Extra", {}, {}, {}, "Extra"});
    QVERIFY(KiraService::Protocol::validateModelRouting(invalid));
    invalid.presets.append({"one-too-many", "Extra", {}, {}, {}, "Extra"});
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.presets[0].model.steps = 201;
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.editingQualities[0].model.steps = 0;
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.editingQualities[2].model.cfgScale = 101;
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.editingQualities[0].isDefault = true;
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid)); // Exactly one default.
    invalid = fixed; invalid.editingQualities[1].isDefault = false;
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    invalid = fixed; invalid.editingQualities[0].name = "Rapid";
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid)); // Qualities stay fixed.
    invalid = fixed; invalid.editingQualities.removeLast();
    QVERIFY(!KiraService::Protocol::validateModelRouting(invalid));
    for (const QJsonValue bad : {QJsonValue(1.5), QJsonValue("4"), QJsonValue(false)}) {
        auto json = KiraService::Protocol::modelRoutingToJson(fixed);
        auto qualities = json.value("editingQualities").toArray();
        auto quality = qualities[0].toObject(); quality.insert("steps", bad); qualities[0] = quality;
        json.insert("editingQualities", qualities);
        QVERIFY(!KiraService::Protocol::modelRoutingFromJson(json));
    }
    // Versions 4 and 5 stored one editing assignment, copied into every quality.
    auto version5 = KiraService::Protocol::modelRoutingToJson(fixed);
    version5.insert("version", 5);
    version5.remove("editingQualities");
    version5.insert("editing", QJsonObject{{"modelId", "image-generation:edit.gguf"}, {"steps", 12}, {"cfgScale", 2.25}});
    const auto copied = KiraService::Protocol::modelRoutingFromJson(version5);
    QVERIFY(copied.has_value());
    for (const auto &quality : copied->editingQualities)
        QCOMPARE(quality.model, (KiraService::ModelAssignment{"image-generation:edit.gguf", 12, 2.25}));
    QCOMPARE(copied->defaultEditingQuality()->id, QString("edit-medium"));
    QTemporaryDir directory;
    QFile weights(directory.filePath("legacy.gguf"));
    QVERIFY(weights.open(QIODevice::WriteOnly)); weights.write("weights"); weights.close();
    ModelCatalog catalog(directory.path(), false);
    QString error;
    auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    options.insert("steps", 37); options.insert("cfgScale", 5.75);
    QVERIFY2(catalog.configure("image-generation:legacy.gguf", options, &error, {}, "Legacy"), qPrintable(error));
    const QJsonObject legacy{{"version", 1}, {"presets", QJsonArray{QJsonObject{
        {"id", "photorealism"}, {"name", "Old name"}, {"positivePrefix", "realism"}, {"negativePrefix", "cartoon"},
        {"qualities", QJsonArray{QJsonObject{{"id", "photo-low"}, {"name", "Low"}, {"modelId", "image-generation:legacy.gguf"}},
                                 QJsonObject{{"id", "photo-medium"}, {"name", "Medium"}, {"modelId", "image-generation:other.gguf"}}}}}}},
        {"editingQualities", QJsonArray{QJsonObject{{"id", "edit-low"}, {"name", "Low"}, {"modelId", "image-generation:legacy.gguf"}}}}};
    QFile file(directory.filePath("model-routing.json"));
    const auto bytes = QJsonDocument(legacy).toJson();
    QVERIFY(file.open(QIODevice::WriteOnly)); file.write(bytes); file.close();
    auto upgraded = catalog.routing(&error);
    QVERIFY2(upgraded.has_value(), qPrintable(error));
    QCOMPARE(upgraded->presets.size(), 2);
    QCOMPARE(upgraded->presets[0].nameFr, QString("Photoréalisme"));
    QCOMPARE(upgraded->presets[0].positivePrefix, QString("realism"));
    QCOMPARE(upgraded->presets[0].model.modelId, QString("image-generation:legacy.gguf"));
    QCOMPARE(upgraded->presets[0].model.steps, std::optional<int>(37));
    QVERIFY(upgraded->presets[1].model.modelId.isEmpty());
    QCOMPARE(upgraded->editingQualities[0].model.cfgScale, std::optional<double>(5.75));
    QVERIFY(upgraded->editingQualities[1].model.modelId.isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes); file.close(); // Reading never rewrites.
    upgraded->presets[0].model.steps = 4;
    QVERIFY(catalog.configureRouting(*upgraded, &error));
    options.insert("steps", 88); options.insert("cfgScale", 9.25);
    QVERIFY(catalog.configure("image-generation:legacy.gguf", options, &error, {}, "Legacy"));
    QCOMPARE(catalog.routing()->presets[0].model.steps, std::optional<int>(4));
    QCOMPARE(catalog.routing()->editingQualities[0].model.steps, std::optional<int>(37));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QCOMPARE(saved.value("version").toInt(), 6);
    QVERIFY(!saved.contains("editing"));
    QCOMPARE(saved.value("editingQualities").toArray().size(), 3);
    QCOMPARE(saved.value("presets").toArray()[0].toObject().value("steps").toInt(), 4);
}

void ServiceTest::configuresModelRoutingThroughSdk()
{
    QTemporaryDir directory;
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    KiraServiceDaemon daemon(configuration);
    QString error; QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    client.connectToService();
    QTRY_VERIFY(client.modelRouting().has_value());
    QVERIFY(client.capabilities().modelRouting);
    KiraService::ServiceClient observer;
    observer.setConfiguration(client.endpoint(), {}); observer.connectToService();
    QTRY_VERIFY(observer.modelRouting().has_value());
    QSignalSpy saved(&client, &KiraService::ServiceClient::modelRoutingConfigured);
    auto routing = *client.modelRouting();
    routing.presets[0].positivePrefix = "realism";
    routing.presets[0].negativePrefix = "illustration";
    QVERIFY(client.configureModelRouting(routing));
    QTRY_COMPARE(saved.size(), 1);
    QTRY_COMPARE(client.modelRouting(), std::optional<KiraService::ModelRouting>(routing));
    QTRY_COMPARE(observer.modelRouting(), std::optional<KiraService::ModelRouting>(routing));
    QVERIFY(!client.configureModelRouting({}));
    routing.presets[1].positivePrefix = "illustrated";
    QVERIFY(client.configureModelRouting(routing));
    QTRY_COMPARE(saved.size(), 2);
    QTRY_COMPARE(client.modelRouting(), std::optional<KiraService::ModelRouting>(routing));
}

void ServiceTest::keepsSingleDefaultPromptLlms()
{
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath("llm"));
    for (const auto &name : {"first.gguf", "second.gguf"}) {
        QFile file(directory.filePath(QString("llm/") + name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("llm");
    }
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    KiraServiceDaemon daemon(configuration);
    QString error; QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client, observer;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    observer.setConfiguration(client.endpoint(), {});
    client.connectToService(); observer.connectToService();
    QTRY_VERIFY(client.modelListAvailable() && observer.modelListAvailable());
    QVERIFY(client.defaultPromptEnhancerModelId().isEmpty());
    QVERIFY(client.defaultPromptTranslatorModelId().isEmpty());
    auto both = KiraService::Protocol::defaultModelOptions("llm-chat");
    QVERIFY(!both.value("defaultPromptEnhancer").toBool() && !both.value("defaultPromptTranslator").toBool());
    both.insert("defaultPromptEnhancer", true); both.insert("defaultPromptTranslator", true);
    QVERIFY(client.configureModel("llm-chat:first.gguf", both, std::nullopt, QString("First")));
    QTRY_COMPARE(observer.defaultPromptEnhancerModelId(), QString("llm-chat:first.gguf"));
    QTRY_COMPARE(observer.defaultPromptTranslatorModelId(), QString("llm-chat:first.gguf"));
    // Taking one role moves only that role; the other stays on the first model.
    auto enhancer = KiraService::Protocol::defaultModelOptions("llm-chat");
    enhancer.insert("defaultPromptEnhancer", true);
    QVERIFY(client.configureModel("llm-chat:second.gguf", enhancer, std::nullopt, QString("Second")));
    QTRY_COMPARE(observer.defaultPromptEnhancerModelId(), QString("llm-chat:second.gguf"));
    QCOMPARE(observer.defaultPromptTranslatorModelId(), QString("llm-chat:first.gguf"));
    ModelCatalog restored(directory.path(), true);
    QVERIFY(!restored.modelById("llm-chat:first.gguf")->recommendedOptions.value("defaultPromptEnhancer").toBool());
    QVERIFY(restored.modelById("llm-chat:first.gguf")->recommendedOptions.value("defaultPromptTranslator").toBool());
    QVERIFY(restored.modelById("llm-chat:second.gguf")->recommendedOptions.value("defaultPromptEnhancer").toBool());
    // Unchecking leaves the role unassigned.
    enhancer.insert("defaultPromptEnhancer", false);
    QVERIFY(restored.configure("llm-chat:second.gguf", enhancer, &error));
    QVERIFY(!restored.modelById("llm-chat:first.gguf")->recommendedOptions.value("defaultPromptEnhancer").toBool());
    enhancer.insert("defaultPromptTranslator", "yes");
    QVERIFY(!KiraService::Protocol::validateModelOptions("llm-chat", enhancer));
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
    QVERIFY(configureNamed(catalog, id, options, &error, true));
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
    QVERIFY(!catalog.configureStyles("image-generation:missing.gguf", styles, &error));
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
    QVERIFY(owner.configureModelStyles("upscaler:test.gguf", styles));
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
    executable.write("#!/bin/sh\n"
                     "printf '%s\\n' \"$@\" > \"$KIRA_TEST_ARGS\"\nprevious=\"\"\nfor argument in "
                     "\"$@\"; do\n if [ \"$previous\" = --output ]; then cp \"$KIRA_TEST_IMAGE\" "
                     "\"$argument\"; fi\n"
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
    const QString editRoot = root;
    QVERIFY(QDir().mkpath(QDir(editRoot).filePath("companions/edit.gguf")));
    for (const QString& name : {QString("edit.gguf"), QString("llm.gguf"), QString("vae.safetensors"), QString("mmproj.gguf")}) {
        QFile file(QDir(editRoot).filePath(name == "edit.gguf" ? name : "companions/edit.gguf/" + name));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
    }
    QFile editMetadata(QDir(editRoot).filePath("metadata.json"));
    QVERIFY(editMetadata.open(QIODevice::ReadOnly));
    auto editEntries = QJsonDocument::fromJson(editMetadata.readAll()).object();
    editMetadata.close();
    editEntries.insert("edit.gguf", QJsonObject{
        {"canEdit", false}, {"modelType", "diffusion"}, {"companionFiles", QJsonObject{
            {"llm", "companions/edit.gguf/llm.gguf"}, {"vae", "companions/edit.gguf/vae.safetensors"}, {"mmproj", "companions/edit.gguf/mmproj.gguf"}}}});
    QVERIFY(editMetadata.open(QIODevice::WriteOnly));
    editMetadata.write(QJsonDocument(editEntries).toJson());
    editMetadata.close();
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
    editOptions.insert("modelId", "image-generation:edit.gguf");
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
    QCOMPARE(editArgs.count("-r"), 1);
    QVERIFY(!editArgs.contains("--ref-image"));
    QCOMPARE(editArgs.count("--offload-to-cpu"), 1);
    QCOMPARE(editArgs.count("--diffusion-fa"), 1);
    QCOMPARE(editArgs.value(editArgs.indexOf("--model-args") + 1), QString("qwen_image_zero_cond_t=true"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--flow-shift") + 1), QString("3"));
    QVERIFY(!editArgs.contains("--init-img"));
    QVERIFY(!editArgs.contains("--strength"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--diffusion-model") + 1), QDir(editRoot).filePath("edit.gguf"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--llm") + 1), QDir(editRoot).filePath("companions/edit.gguf/llm.gguf"));
    QCOMPARE(editArgs.value(editArgs.indexOf("--vae") + 1), QDir(editRoot).filePath("companions/edit.gguf/vae.safetensors"));
    QVERIFY(!editArgs.contains("--llm_vision"));
    QCOMPARE(messages.last().value("operation").toString(), QString("image.edit"));
    images.clear();
    messages.clear();
    editOptions.insert("inputCount", 3);
    socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "submit"}, {"operation", "image.edit"}, {"jobId", "edit-multi"},
        {"options", editOptions}}).toJson()));
    for (int index = 0; index < 3; ++index) {
        QTRY_COMPARE(std::count_if(messages.cbegin(), messages.cend(), [](const QJsonObject &message) {
            return message.value("type") == "upload-required" && message.value("jobId") == "edit-multi"; }), index + 1);
        socket.sendBinaryMessage(image);
    }
    QTRY_VERIFY(terminal("edit-multi"));
    QTRY_COMPARE(images.count(), 1);
    QCOMPARE(arguments().count("-r"), 3);
    images.clear();
    messages.clear();
    // Generation accepts reference images without a base image.
    QJsonObject referenceOptions = valid;
    referenceOptions.insert("inputCount", 2);
    send("generate-references", referenceOptions);
    for (int index = 0; index < 2; ++index) {
        QTRY_COMPARE(std::count_if(messages.cbegin(), messages.cend(), [](const QJsonObject &message) {
            return message.value("type") == "upload-required" && message.value("jobId") == "generate-references"
                && message.value("inputRole") == "reference"; }), index + 1);
        socket.sendBinaryMessage(image);
    }
    QTRY_VERIFY(terminal("generate-references"));
    QTRY_COMPARE(images.count(), 1);
    QCOMPARE(arguments().count("-r"), 2);
    QVERIFY(!arguments().contains("--init-img"));
    referenceOptions.insert("hasInput", true);
    send("generate-references-base", referenceOptions);
    QTRY_VERIFY(terminal("generate-references-base"));
    QCOMPARE(messages.last().value("code").toString(), QString("invalid-request"));
    images.clear();
    messages.clear();
    send("automatic", valid);
    QTRY_VERIFY(terminal("automatic"));
    QTRY_COMPARE(images.count(), 1);
    QCOMPARE(images.last().first().toByteArray(), image);
    auto args = arguments();
    QCOMPARE(args.count("--offload-to-cpu"), 1);
    QCOMPARE(args.count("--diffusion-fa"), 1);
    QVERIFY(!args.contains("--clip_vision"));
    QVERIFY(!args.contains("--sampling-method"));
    QVERIFY(!args.contains("--scheduler"));
    QVERIFY(!args.contains("--clip-skip"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("5.75"));
    QCOMPARE(args.value(args.indexOf("--steps") + 1), QString("37"));
    QCOMPARE(args.value(args.indexOf("--vae") + 1), QDir(root).filePath(companion));
    QVERIFY(args.contains("--model"));
    QVERIFY(!args.contains("--diffusion-model"));
    // Resolve shared installed VAE and LLM assignments without changing job options.
    QVERIFY(QDir().mkpath(QDir(root).filePath("companion")));
    QVERIFY(QDir().mkpath(QDir(root).filePath("llm")));
    QVERIFY(QFile::copy(QDir(root).filePath(companion), QDir(root).filePath("companion/shared-vae.gguf")));
    QVERIFY(QFile::copy(QDir(root).filePath(llm), QDir(root).filePath("llm/shared-encoder.gguf")));
    QFile sharedMetadata(QDir(root).filePath("companion/metadata.json"));
    QVERIFY(sharedMetadata.open(QIODevice::WriteOnly));
    sharedMetadata.write(QJsonDocument(QJsonObject{{"shared-vae.gguf", QJsonObject{{"recommendedOptions", QJsonObject{{"subtype", "vae"}}}}}}).toJson());
    sharedMetadata.close();
    QFile encoderMetadata(QDir(root).filePath("llm/metadata.json"));
    QVERIFY(encoderMetadata.open(QIODevice::WriteOnly));
    encoderMetadata.write(QJsonDocument(QJsonObject{{"shared-encoder.gguf", QJsonObject{{"recommendedOptions", QJsonObject{{"canBeTextEncoder", true}}}}}}).toJson());
    encoderMetadata.close();
    QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    metadata.write(QJsonDocument(QJsonObject{{"model.gguf", QJsonObject{
        {"recommendedOptions", QJsonObject{{"canGenerate", true}, {"canEdit", false},
            {"vaeModelId", "companion:shared-vae.gguf"}, {"textEncoderModelId", "llm-chat:shared-encoder.gguf"}}},
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
    QVERIFY(!args.contains("--clip_l"));
    QVERIFY(!args.contains("--clip_g"));
    QCOMPARE(args.value(args.indexOf("--llm") + 1), QDir(root).filePath("llm/shared-encoder.gguf"));
    QVERIFY(!args.contains("--llm_vision"));
    QCOMPARE(args.value(args.indexOf("--vae") + 1), QDir(root).filePath("companion/shared-vae.gguf"));
    KiraService::ServiceClient capabilityClient;
    capabilityClient.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())), {});
    capabilityClient.connectToService();
    QTRY_VERIFY(capabilityClient.modelListAvailable());
    QVERIFY(capabilityClient.canSubmit(KiraService::ImageOperation::Generate, "image-generation:model.gguf"));
    QVERIFY(!capabilityClient.canSubmit(KiraService::ImageOperation::Edit, "image-generation:model.gguf"));
    ModelCatalog capabilityCatalog(root, true);
    auto capabilities = KiraService::Protocol::defaultModelOptions("image-generation");
    capabilities.insert("canGenerate", false);
    capabilities.insert("canEdit", true);
    capabilities.insert("vaeModelId", "companion:shared-vae.gguf");
    capabilities.insert("textEncoderModelId", "llm-chat:shared-encoder.gguf");
    QVERIFY(configureNamed(capabilityCatalog, "image-generation:model.gguf", capabilities, &error));
    capabilityClient.requestModels();
    QTRY_VERIFY(!capabilityClient.canSubmit(KiraService::ImageOperation::Generate, "image-generation:model.gguf"));
    QVERIFY(capabilityClient.canSubmit(KiraService::ImageOperation::Edit, "image-generation:model.gguf"));
    messages.clear(); send("disabled-generation", valid);
    QTRY_VERIFY(terminal("disabled-generation"));
    QCOMPARE(messages.last().value("code").toString(), QString("model-unavailable"));
    capabilities.insert("canGenerate", true);
    QVERIFY(configureNamed(capabilityCatalog, "image-generation:model.gguf", capabilities, &error));
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
    QCOMPARE(args.value(args.indexOf("--vae") + 1), QDir(root).filePath("companion/shared-vae.gguf"));
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
    for (const QJsonValue style : {QJsonValue(42), QJsonValue("cinematic")}) {
        auto invalidStyled = valid;
        invalidStyled.insert("styleId", style);
        if (style.isString()) invalidStyled.insert("prompt", " ");
        messages.clear(); send("invalid-style", invalidStyled);
        QTRY_VERIFY(terminal("invalid-style"));
        QCOMPARE(messages.last().value("code").toString(), QString("invalid-request"));
    }
    auto routing = KiraService::Protocol::starterModelRouting();
    routing.presets[0].positivePrefix = "  realism  ";
    routing.presets[0].negativePrefix = "  cartoon  ";
    routing.presets[0].model = {"image-generation:plain.gguf", 4, 1.0};
    routing.editingQualities[2].model = {"image-generation:edit.gguf", 12, 2.25};
    routing.editingQualities[2].positivePrefix = "  sharp details  ";
    routing.editingQualities[2].negativePrefix = "  blurry  ";
    QVERIFY(styleCatalog.configureRouting(routing, &error));
    const QJsonObject routed{{"presetId", "photorealism"},
        {"prompt", "  subject  "}, {"negativePrompt", "  noise  "}, {"hasInput", true}};
    messages.clear(); send("routing-snapshot", routed);
    const auto requestedUpload = [&](const QString &id) {
        return std::any_of(messages.cbegin(), messages.cend(), [&](const QJsonObject &m) {
            return m.value("jobId") == id && m.value("type") == "upload-required";
        });
    };
    QTRY_VERIFY(requestedUpload("routing-snapshot"));
    routing.presets[0].positivePrefix = "changed";
    routing.presets[0].model = {"image-generation:model.gguf", 50, 8.0};
    QVERIFY(styleCatalog.configureRouting(routing, &error));
    socket.sendBinaryMessage(image);
    QTRY_VERIFY(terminal("routing-snapshot"));
    ++completed;
    QTRY_COMPARE(images.count(), completed);
    args = arguments();
    QCOMPARE(args.value(args.indexOf("--prompt") + 1), QString("realism, subject"));
    QCOMPARE(args.value(args.indexOf("--steps") + 1), QString("4"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("1"));
    QCOMPARE(args.value(args.indexOf("--negative-prompt") + 1), QString("cartoon, noise"));
    QCOMPARE(args.value(args.indexOf("--model") + 1), QDir(root).filePath("plain.gguf"));
    for (const auto &field : QList<QPair<QString, QJsonValue>>{
        {"prompt", " "}, {"qualityId", "photo-medium"}, {"presetId", ""}, {"presetId", false},
        {"presetId", "missing"}, {"styleId", "cinematic"}, {"modelId", "image-generation:plain.gguf"},
        {"steps", 0}, {"cfgScale", 101}}) {
        auto invalidRoute = routed; invalidRoute.insert(field.first, field.second);
        messages.clear(); send("invalid-route", invalidRoute);
        QTRY_VERIFY(terminal("invalid-route"));
        QCOMPARE(messages.last().value("type").toString(), QString("error"));
    }
    // Editing is routed by quality: the service resolves its model and fills Steps/CFG.
    const auto sendEdit = [&](const QString &id, const QJsonObject &options) {
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "submit"}, {"operation", "image.edit"}, {"jobId", id}, {"options", options}}).toJson()));
    };
    messages.clear();
    sendEdit("quality-edit", {{"qualityId", "edit-high"}, {"prompt", "change background"}, {"hasInput", true}});
    QTRY_VERIFY(requestedUpload("quality-edit"));
    socket.sendBinaryMessage(image);
    QTRY_VERIFY(terminal("quality-edit"));
    ++completed;
    QTRY_COMPARE(images.count(), completed);
    args = arguments();
    QVERIFY(args.contains(QDir(root).filePath("edit.gguf")));
    QCOMPARE(args.value(args.indexOf("--steps") + 1), QString("12"));
    QCOMPARE(args.value(args.indexOf("--cfg-scale") + 1), QString("2.25"));
    // The quality's prefixes lead the prompts sent to sd-cli.
    QCOMPARE(args.value(args.indexOf("--prompt") + 1), QString("sharp details, change background"));
    QCOMPARE(args.value(args.indexOf("--negative-prompt") + 1), QString("blurry"));
    for (const auto &field : QList<QPair<QString, QJsonValue>>{
        {"presetId", "photorealism"}, {"modelId", "image-generation:plain.gguf"}, {"qualityId", ""},
        {"qualityId", false}, {"styleId", "cinematic"}}) {
        auto invalidEdit = QJsonObject{{"qualityId", "edit-high"}, {"prompt", "change background"}, {"hasInput", true}};
        invalidEdit.insert(field.first, field.second);
        messages.clear(); sendEdit("routed-edit", invalidEdit);
        QTRY_VERIFY(terminal("routed-edit"));
        QCOMPARE(messages.last().value("code").toString(), QString("invalid-request"));
    }
    for (const QString quality : {"edit-low", "missing"}) { // Unassigned or unknown quality.
        messages.clear();
        sendEdit("unrouted-edit", {{"qualityId", quality}, {"prompt", "change background"}, {"hasInput", true}});
        QTRY_VERIFY(terminal("unrouted-edit"));
        QCOMPARE(messages.last().value("code").toString(), QString("model-routing-unavailable"));
    }
    QVERIFY(styleCatalog.configureRouting(KiraService::Protocol::starterModelRouting(), &error));

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
    QVERIFY(detail.contains(QDir(root).filePath("llm/shared-encoder.gguf")));
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

void ServiceTest::reportsTheRunningSamplingStep()
{
    using Stage = KiraService::GenerationProgressStage;
    SdCliProgressParser parser;
    parser.begin(4);
    QVERIFY(parser.processLine("generating image: 1/1 - seed 42").has_value());
    // The first step starts right after "sampling using": 1 / 4 is running.
    auto progress = parser.processLine("sampling using Euler method");
    QVERIFY(progress && progress->stage == Stage::Sampling);
    QCOMPARE(progress->currentStep, std::optional<int>(1));
    QCOMPARE(progress->percentage, 15);
    // sd-cli counts completed steps; the running one is shown.
    progress = parser.processPartialLine("  |=====>            | 1/4 - 1.00s/it\x1b[K");
    QVERIFY(progress);
    QCOMPARE(progress->currentStep, std::optional<int>(2));
    QCOMPARE(progress->totalSteps, std::optional<int>(4));
    QCOMPARE(progress->percentage, 34);
    // The same counter arriving again when its line ends is not repeated.
    QVERIFY(!parser.processLine("  |=====>            | 1/4 - 1.00s/it\x1b[K"));
    // A counter cut by a read is ignored until it is complete.
    QVERIFY(!parser.processPartialLine("  |==========>       | 2/"));
    QVERIFY(!parser.processPartialLine("  |==========>       | 2/4"));
    progress = parser.processPartialLine("  |===============>  | 3/4 - 1.00s/it\x1b[K");
    QCOMPARE(progress->currentStep, std::optional<int>(4)); // The last step shows 4 / 4 while it runs.
    QCOMPARE(progress->percentage, 71);
    progress = parser.processLine("  |====================| 4/4 - 1.00s/it\x1b[K");
    QCOMPARE(progress->currentStep, std::optional<int>(4));
    QCOMPARE(progress->percentage, 90);
    QVERIFY(!parser.processLine("sampling completed, taking 4.00s"));
    QCOMPARE(parser.processLine("decoding 1 latents")->stage, Stage::Decoding);
}

void ServiceTest::readsProgressBeforeTheLineEnds()
{
#ifdef Q_OS_WIN
    QSKIP("The fake sd-cli fixture requires a POSIX shell.");
#endif
    QTemporaryDir directory;
    QFile executable(directory.filePath("sd-cli"));
    QVERIFY(executable.open(QIODevice::WriteOnly));
    // Like sd-cli, redraw the bar with a leading carriage return and no line end, then keep sampling.
    executable.write("#!/bin/sh\nprintf 'generating image: 1/1 - seed 1\\n'\nprintf 'sampling using Euler method\\n'\n"
                     "printf '\\r  |=====>     | 1/2 - 1.00s/it\\033[K'\nexec sleep 30\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    SdCliRunner runner(executable.fileName());
    QList<KiraService::GenerationProgress> progress;
    connect(&runner, &SdCliRunner::generationProgressChanged, this,
            [&](const KiraService::GenerationProgress &value) { progress.append(value); });
    GenerationRequest request;
    request.steps = 2;
    request.modelPath = directory.filePath("model.gguf");
    request.outputPath = directory.filePath("output.png");
    QVERIFY(runner.runGeneration(request));
    // Step 1 is done and step 2 runs while sd-cli is still sampling, not after its next redraw.
    QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty() && progress.last().currentStep == 2, 10000);
    QCOMPARE(progress.last().totalSteps, std::optional<int>(2));
    QSignalSpy finished(&runner, &SdCliRunner::generationFinished);
    runner.cancel();
    QTRY_VERIFY(!finished.isEmpty());
}

void ServiceTest::validatesRecommendedOptions()
{
    using namespace KiraService::Protocol;
    for (const auto& category : {"image-generation", "llm-chat", "upscaler"}) {
        auto options = defaultModelOptions(category);
        QVERIFY(validateModelOptions(category, options));
        options.insert("unrelated", true);
        QVERIFY(!validateModelOptions(category, options));
    }
    auto image = defaultModelOptions("image-generation");
    QVERIFY(!image.contains("clipSkip"));
    image.insert("canEdit", false); image.insert("canGenerate", false);
    QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("canGenerate", true); QVERIFY(validateModelOptions("image-generation", image));
    image.insert("canEdit", "yes"); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("canEdit", true);
    image.insert("vaeModelId", 1); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("vaeModelId", "");
    image.insert("steps", "28"); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("steps", 28.5); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("steps", 200); image.insert("width", 2048); image.insert("height", 2048); QVERIFY(validateModelOptions("image-generation", image));
    image.insert("clipSkip", 0); QVERIFY(!validateModelOptions("image-generation", image));
    image.insert("clipSkip", 12); image.insert("sampler", "unknown"); QVERIFY(!validateModelOptions("image-generation", image));
    // The prompt format defaults to phrases, is optional for existing models and accepts only known formats.
    auto format = defaultModelOptions("image-generation");
    QCOMPARE(format.value("promptFormat").toString(), QString("phrases"));
    for (const auto &value : {"natural", "phrases", "tags"}) {
        format.insert("promptFormat", value); QVERIFY(validateModelOptions("image-generation", format));
    }
    format.insert("promptFormat", "prose"); QVERIFY(!validateModelOptions("image-generation", format));
    format.insert("promptFormat", 1); QVERIFY(!validateModelOptions("image-generation", format));
    format.remove("promptFormat"); QVERIFY(validateModelOptions("image-generation", format));
    QCOMPARE(KiraService::ModelInfo{}.promptFormat(), QString("phrases"));
    auto llm = defaultModelOptions("llm-chat");
    llm.insert("canBeTextEncoder", "yes"); QVERIFY(!validateModelOptions("llm-chat", llm));
    llm.insert("canBeTextEncoder", true); QVERIFY(validateModelOptions("llm-chat", llm));
    llm.insert("maxTokens", 4097); QVERIFY(!validateModelOptions("llm-chat", llm));
    llm.insert("maxTokens", 4096); llm.insert("topP", 0); QVERIFY(!validateModelOptions("llm-chat", llm));
    QVERIFY(validateModelOptions("upscaler", {}));
    QVERIFY(!validateModelOptions("upscaler", {{"tileSize", 129}}));
}

void ServiceTest::assignsImageReferencesAndProtectsSharedModels()
{
    QTemporaryDir directory;
    TestHttpServer source("weights"); QVERIFY(source.start());
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy installed(&catalog, &ModelCatalog::installFinished);
    QString error;
    ModelInstallRequest request;
    request.installId = "vae"; request.category = ModelCategory::Companion;
    request.url = source.url("vae.gguf"); request.displayName = "VAE";
    request.recommendedOptions = QJsonObject{{"subtype", "vae"}};
    QVERIFY(catalog.install(request, &error)); QTRY_COMPARE(installed.count(), 1);
    request.installId = "encoder"; request.category = ModelCategory::LlmChat;
    request.url = source.url("encoder.gguf"); request.displayName = "Encoder";
    auto llm = KiraService::Protocol::defaultModelOptions("llm-chat");
    llm.insert("canBeTextEncoder", true); request.recommendedOptions = llm;
    QVERIFY(catalog.install(request, &error)); QTRY_COMPARE(installed.count(), 2);
    request.installId = "image"; request.category = ModelCategory::ImageGeneration;
    request.url = source.url("image.gguf"); request.displayName = "Image";
    auto image = KiraService::Protocol::defaultModelOptions("image-generation");
    image.insert("canGenerate", false);
    image.insert("vaeModelId", "llm-chat:encoder.gguf"); request.recommendedOptions = image;
    QVERIFY(!catalog.install(request, &error));
    image.insert("vaeModelId", "companion:vae.gguf");
    image.insert("textEncoderModelId", "llm-chat:encoder.gguf"); request.recommendedOptions = image;
    QVERIFY(catalog.install(request, &error));
    QVERIFY(!catalog.remove("companion:vae.gguf", &error));
    QTRY_COMPARE(installed.count(), 3);
    ModelCatalog restored(directory.path(), true);
    QCOMPARE(restored.modelById("image-generation:image.gguf")->recommendedOptions, image);
    QVERIFY(!restored.remove("companion:vae.gguf", &error));
    QVERIFY(!restored.remove("llm-chat:encoder.gguf", &error));
    QVERIFY(!restored.configure("companion:vae.gguf", {{"subtype", "mmproj"}}, &error));
    llm.insert("canBeTextEncoder", false);
    QVERIFY(!restored.configure("llm-chat:encoder.gguf", llm, &error));
    image.insert("vaeModelId", ""); image.insert("textEncoderModelId", "");
    QVERIFY(restored.configure("image-generation:image.gguf", image, &error));
    QVERIFY(restored.configure("llm-chat:encoder.gguf", llm, &error));
    QVERIFY(restored.remove("companion:vae.gguf", &error));
    QVERIFY(QFileInfo::exists(directory.filePath("image.gguf")));
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
    QVERIFY2(configureNamed(catalog, id, options, &error, true), qPrintable(error));
    QVERIFY(catalog.modelById(id)->explicitContent);
    QVERIFY(configureNamed(catalog, id, options, &error));
    QVERIFY(catalog.modelById(id)->explicitContent);
    ModelCatalog restored(directory.path(), true); QCOMPARE(restored.modelById(id)->recommendedOptions, options);
    QVERIFY(restored.modelById(id)->explicitContent);
    QVERIFY(configureNamed(restored, id, options, &error, false));
    QVERIFY(!catalog.modelById(id)->explicitContent);
    QCOMPARE(restored.modelById(id)->companionFileName, QString("companions/legacy.gguf/vae.gguf"));
    QVERIFY(file.open(QIODevice::ReadOnly)); auto metadata = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    QVERIFY(metadata.value("untouched.gguf").isString());
    auto bad = options; bad.insert("steps", -1); QVERIFY(!configureNamed(catalog, id, bad, &error)); QCOMPARE(catalog.modelById(id)->recommendedOptions, options);
    QVERIFY(restored.removeCompanion(id, "vae", &error));
    QVERIFY(restored.modelById(id));
    QVERIFY(restored.modelById(id)->companionFileName.isEmpty());
    QCOMPARE(restored.modelById(id)->recommendedOptions, options);
    QVERIFY(!QFileInfo::exists(directory.filePath("companions/legacy.gguf/vae.gguf")));
    QVERIFY(restored.remove(id, &error)); QVERIFY(file.open(QIODevice::ReadOnly)); metadata = QJsonDocument::fromJson(file.readAll()).object(); file.close(); QVERIFY(!metadata.contains("legacy.gguf"));
    QFile legacy(directory.filePath("legacy.gguf")); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write("model"); legacy.close();
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("broken json"); file.close();
    QVERIFY(!configureNamed(catalog, id, options, &error)); QVERIFY(!catalog.remove(id, &error));
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("broken json")); QVERIFY(QFileInfo::exists(legacy.fileName()));
}

void ServiceTest::reservesSingleInstallAndCancels()
{
    TestHttpServer server("model"); QVERIFY(server.start()); QTemporaryDir directory; ModelCatalog catalog(directory.path(), true);
    QSignalSpy canceled(&catalog, &ModelCatalog::installCanceled); QSignalSpy completed(&catalog, &ModelCatalog::installFinished); QString error;
    QVERIFY(installNamed(catalog, {"first", ModelCategory::Upscaler, server.url(), {}}, &error)); QVERIFY(catalog.isInstalling());
    QVERIFY(!installNamed(catalog, {"second", ModelCategory::Upscaler, server.url("other.gguf"), {}}, &error));
    catalog.cancel("first"); QCOMPARE(canceled.count(), 1); QVERIFY(!catalog.isInstalling());
    QVERIFY(installNamed(catalog, {"third", ModelCategory::Upscaler, server.url(), {}, KiraService::Protocol::defaultModelOptions("upscaler")}, &error));
    QTRY_COMPARE(completed.count(), 1); QCOMPARE(catalog.models(ModelCategory::Upscaler).first().recommendedOptions, KiraService::Protocol::defaultModelOptions("upscaler"));
    QVERIFY(!QFileInfo::exists(directory.filePath("upscale/model.gguf.part")));
}

void ServiceTest::cancelsDuringCompanionDownload()
{
    TestHttpServer server("model"); QVERIFY(server.start()); QTemporaryDir directory; ModelCatalog catalog(directory.path(), true);
    QSignalSpy canceled(&catalog, &ModelCatalog::installCanceled); QSignalSpy completed(&catalog, &ModelCatalog::installFinished); QSignalSpy progress(&catalog, &ModelCatalog::installProgress); QString error;
    QVERIFY(installNamed(catalog, {"pair", ModelCategory::ImageGeneration, server.url(), server.url("slow.gguf"), KiraService::Protocol::defaultModelOptions("image-generation")}, &error));
    const auto companionStarted = [&progress] {
        for (const auto &event : progress) if (event.first().value<ModelInstallProgress>().fileRole == "companion") return true;
        return false;
    };
    QTRY_VERIFY(companionStarted());
    catalog.cancel("pair"); QCOMPARE(canceled.count(), 1); QCOMPARE(completed.count(), 0); QVERIFY(!catalog.isInstalling());
    QVERIFY(!QFileInfo::exists(directory.filePath("model.gguf.part"))); QVERIFY(!QFileInfo::exists(directory.filePath("companions/model.gguf/slow.gguf.part"))); QVERIFY(catalog.models(ModelCategory::ImageGeneration).isEmpty());
}

void ServiceTest::rollsBackMetadataFailure()
{
    TestHttpServer server("model"); QVERIFY(server.start()); QTemporaryDir directory; ModelCatalog catalog(directory.path(), true);
    QSignalSpy failed(&catalog, &ModelCatalog::installFailed); QSignalSpy completed(&catalog, &ModelCatalog::installFinished); QString error;
    QVERIFY(installNamed(catalog, {"broken", ModelCategory::ImageGeneration, server.url(), server.url("vae.gguf"), KiraService::Protocol::defaultModelOptions("image-generation")}, &error));
    QVERIFY(QDir().mkpath(directory.filePath("metadata.json")));
    QTRY_COMPARE(failed.count(), 1); QCOMPARE(completed.count(), 0); QVERIFY(catalog.models(ModelCategory::ImageGeneration).isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath("model.gguf"))); QVERIFY(!QFileInfo::exists(directory.filePath("companions/model.gguf/vae.gguf")));
}

void ServiceTest::configuresModelsOverProtocol()
{
    QTemporaryDir directory; QFile model(directory.filePath("test.gguf")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
    KiraServiceConfiguration configuration; configuration.port = 0; configuration.modelRootPath = directory.path(); KiraServiceDaemon service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
    QWebSocket client; QSignalSpy received(&client, &QWebSocket::textMessageReceived); client.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort()))); QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
    const auto send = [&client](const QJsonObject &object) { client.sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))); };
    send({{"type", "authenticate"}, {"version", 3}}); QTRY_VERIFY(received.count() >= 2); received.clear();
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    send({{"type", "models.configure"}, {"displayName", "Fixture model"}, {"id", "image-generation:test.gguf"}, {"recommendedOptions", options}, {"explicitContent", true}});
    QTRY_COMPARE(received.count(), 1);
    auto response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object();
    QCOMPARE(response.value("type").toString(), QString("models.configure.completed"));
    QCOMPARE(response.value("model").toObject().value("recommendedOptions").toObject(), options);
    QVERIFY(response.value("model").toObject().value("explicitContent").toBool());
    QVERIFY(!response.value("model").toObject().contains("filePath"));
    send({{"type", "models.list"}});
    QTRY_COMPARE(received.count(), 1);
    response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object();
    QCOMPARE(response.value("categories").toArray().size(), 4);
    QCOMPARE(response.value("categories").toArray().first().toObject().value("models").toArray().first().toObject().value("recommendedOptions").toObject(), options);
    for (const auto& invalid : {QJsonObject{{"explicitContent", "true"}}, QJsonObject{{"recommendedOptions", true}}}) {
        QJsonObject message{{"type", "models.configure"}, {"displayName", "Fixture model"}, {"id", "image-generation:test.gguf"}, {"recommendedOptions", options}};
        for (auto it = invalid.begin(); it != invalid.end(); ++it) message.insert(it.key(), it.value());
        send(message);
        QTRY_COMPARE(received.count(), 1);
        response = QJsonDocument::fromJson(received.takeFirst().first().toString().toUtf8()).object();
        QCOMPARE(response.value("code").toString(), QString("invalid-model-configure"));
    }
    QTcpServer stalledDownload; QVERIFY(stalledDownload.listen(QHostAddress::LocalHost));
    const QString url = QString("https://127.0.0.1:%1/one.gguf").arg(stalledDownload.serverPort());
    QWebSocket stranger; QSignalSpy strangerMessages(&stranger, &QWebSocket::textMessageReceived);
    stranger.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort()))); QTRY_COMPARE(stranger.state(), QAbstractSocket::ConnectedState);
    stranger.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}"); QTRY_VERIFY(strangerMessages.count() >= 2); strangerMessages.clear();
    send({{"type", "models.install"}, {"displayName", "Fixture model"}, {"installId", "one"}, {"category", "upscaler"}, {"url", url}});
    const auto contains = [](const QSignalSpy &spy, const QString &key, const QString &value) {
        for (const auto &message : spy) if (QJsonDocument::fromJson(message.first().toString().toUtf8()).object().value(key).toString() == value) return true;
        return false;
    };
    QTRY_VERIFY(contains(received, "type", "models.install.accepted"));
    stranger.sendTextMessage("{\"type\":\"models.install.cancel\",\"installId\":\"one\"}"); QTRY_VERIFY(contains(strangerMessages, "code", "invalid-model-cancel"));
    send({{"type", "models.install"}, {"displayName", "Fixture model"}, {"installId", "two"}, {"category", "upscaler"}, {"url", url}}); QTRY_VERIFY(contains(received, "code", "model-install-busy"));
    send({{"type", "models.install.cancel"}, {"installId", "one"}}); QTRY_VERIFY(contains(received, "type", "models.install.canceled"));
    send({{"type", "models.install"}, {"displayName", "Fixture model"}, {"installId", "three"}, {"category", "upscaler"}, {"url", url}});
    QTRY_VERIFY(contains(received, "installId", "three")); client.close(); QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
    stranger.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "models.install"}, {"displayName", "Fixture model"}, {"installId", "four"}, {"category", "upscaler"}, {"url", url}}).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY(contains(strangerMessages, "type", "models.install.accepted")); stranger.close();

}

void ServiceTest::preservesProtocolV3()
{
    QCOMPARE(KiraService::Protocol::Version, 3);
    QCOMPARE(KiraService::Protocol::DefaultPort, quint16(9384));
}

void ServiceTest::requiresNamesAndAssignsSharedMmproj()
{
    TestHttpServer source("weights");
    QVERIFY(source.start());
    QTemporaryDir directory;
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy installed(&catalog, &ModelCatalog::installFinished);
    QString error;
    ModelInstallRequest companion;
    companion.installId = "projector";
    companion.category = ModelCategory::Companion;
    companion.url = source.url("projector.gguf");
    companion.recommendedOptions = QJsonObject{{"subtype", "mmproj"}};
    QVERIFY(!catalog.install(companion, &error));
    QVERIFY(error.contains("display name"));
    companion.displayName = "  ";
    QVERIFY(!catalog.install(companion, &error));
    companion.displayName = "Vision projector";
    QVERIFY2(catalog.install(companion, &error), qPrintable(error));
    QTRY_COMPARE(installed.count(), 1);
    const QString projectorId = "companion:projector.gguf";
    QCOMPARE(catalog.modelById(projectorId)->recommendedOptions.value("subtype").toString(), QString("mmproj"));
    companion.installId = "vae";
    companion.displayName = "Autoencoder";
    companion.url = source.url("vae.safetensors");
    companion.recommendedOptions = QJsonObject{{"subtype", "vae"}};
    QVERIFY(catalog.install(companion, &error));
    QTRY_COMPARE(installed.count(), 2);
    ModelInstallRequest llm;
    llm.installId = "llm";
    llm.category = ModelCategory::LlmChat;
    llm.url = source.url("chat.gguf");
    llm.displayName = "Chat";
    llm.recommendedOptions = KiraService::Protocol::defaultModelOptions("llm-chat");
    llm.mmprojModelId = "companion:vae.safetensors";
    QVERIFY(!catalog.install(llm, &error));
    llm.mmprojModelId = projectorId;
    QVERIFY(catalog.install(llm, &error));
    QTRY_COMPARE(installed.count(), 3);
    ModelCatalog restored(directory.path(), true);
    QCOMPARE(restored.modelById("llm-chat:chat.gguf")->mmprojModelId, projectorId);
    QVERIFY(!restored.remove(projectorId, &error));
    QVERIFY(QFileInfo::exists(directory.filePath("companion/projector.gguf")));
    QVERIFY(!restored.configure(projectorId, {{"subtype", "vae"}}, &error));
    QVERIFY(!restored.configure("llm-chat:chat.gguf", *llm.recommendedOptions, &error, {}, QString()));
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = directory.path();
    KiraServiceDaemon daemon(configuration);
    QVERIFY(daemon.start());
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    client.connectToService();
    QTRY_COMPARE(client.models(KiraService::ModelCategory::Companion).size(), 2);
    QCOMPARE(client.models(KiraService::ModelCategory::LlmChat).first().mmprojModelId, projectorId);
    QVERIFY(client.configureModel("llm-chat:chat.gguf", *llm.recommendedOptions, {}, {}, QString()));
    QTRY_VERIFY(client.models(KiraService::ModelCategory::LlmChat).first().mmprojModelId.isEmpty());
    QVERIFY(restored.remove(projectorId, &error));
    QVERIFY(QFileInfo::exists(directory.filePath("llm/chat.gguf")));
    QVERIFY(QFileInfo::exists(directory.filePath("companion/vae.safetensors")));
}

void ServiceTest::rejectsRetiredCategoriesAndOperations()
{
    const QStringList retired{"prompt-enhancer", "clip_vision", "inpaint", "face-parsing", "edition"};
    QTemporaryDir directory;
    for (const auto& category : retired) {
        QVERIFY(QDir().mkpath(directory.filePath(category)));
        QFile file(directory.filePath(category + "/model.gguf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("existing model");
        QVERIFY(!ModelCatalog::categoryFromName(category));
        QVERIFY(KiraService::Protocol::defaultModelOptions(category).isEmpty());
    }
    ModelCatalog catalog(directory.path(), false);
    QCOMPARE(ModelCatalog::categories().size(), 4);
    for (const auto& category : retired)
        QVERIFY(QFileInfo::exists(directory.filePath(category + "/model.gguf")));
    KiraServiceConfiguration configuration;
    configuration.port = 0;
    configuration.modelRootPath = directory.path();
    KiraServiceDaemon service(configuration);
    QVERIFY(service.start());
    QWebSocket client;
    QSignalSpy messages(&client, &QWebSocket::textMessageReceived);
    client.open(QUrl(QString("ws://127.0.0.1:%1").arg(service.serverPort())));
    QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
    client.sendTextMessage("{\"type\":\"authenticate\",\"version\":3}");
    QTRY_VERIFY(messages.count() >= 2);
    for (const auto& operation : {"image.inpaint", "image.face-parsing"}) {
        messages.clear();
        client.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "submit"}, {"operation", operation}, {"jobId", "retired"},
            {"options", QJsonObject{}}}).toJson()));
        QTRY_COMPARE(messages.count(), 1);
        const auto error = QJsonDocument::fromJson(messages.first().first().toString().toUtf8()).object();
        QCOMPARE(error.value("code").toString(), QString("invalid-request"));
    }
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
    request.explicitContent = false;
    QVERIFY(installNamed(catalog, request, &error));
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!catalog.modelById("upscaler:model.gguf")->explicitContent);
    ModelCatalog restored(directory.path(), true);
    QVERIFY(!restored.modelById("upscaler:model.gguf")->explicitContent);
    const QString finalPath = directory.filePath(QStringLiteral("upscale/model.gguf"));
    QVERIFY(QFileInfo::exists(finalPath)); QVERIFY(!QFileInfo::exists(finalPath + QStringLiteral(".part")));
}

void ServiceTest::routesPresetGenerationWithoutLlm()
{
#ifdef Q_OS_WIN
    QSKIP("The fake sd-cli and llama-completion fixtures require a POSIX shell.");
#endif
    QTemporaryDir directory;
    const QByteArray oldPath = qgetenv("PATH");
    const auto restore = qScopeGuard([oldPath] { qputenv("PATH", oldPath); });
    qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
    QFile image(directory.filePath("fixture.png"));
    QVERIFY(image.open(QIODevice::WriteOnly));
    image.write(QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1kAAAAASUVORK5CYII="));
    image.close();
    const auto writeScript = [&](const QString &name, const QByteArray &body) {
        QFile script(directory.filePath(name));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(body);
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    };
    writeScript("sd-cli", "#!/bin/sh\nroot=$(dirname \"$0\")\nprintf '%s\\n' \"$@\" > \"$root/sd-args\"\nprevious=\n"
                          "for argument in \"$@\"; do\n"
                          "if [ \"$previous\" = --output ]; then cp \"$root/fixture.png\" \"$argument\"; fi\n"
                          "previous=\"$argument\"\ndone\n");
    // Generation never calls an LLM; any call leaves a marker.
    writeScript("completion", "#!/bin/sh\ntouch \"$(dirname \"$0\")/llm-called\"\nprintf 'unexpected'\n");
    const QString root = directory.filePath("models");
    QVERIFY(QDir().mkpath(QDir(root).filePath("llm")));
    for (const auto &name : {QString("image.gguf"), QString("llm/chat.gguf")}) {
        QFile file(QDir(root).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
    }
    ModelCatalog catalog(root, false);
    QString error;
    QVERIFY2(configureNamed(catalog, "image-generation:image.gguf",
        KiraService::Protocol::defaultModelOptions("image-generation"), &error), qPrintable(error));
    QVERIFY2(configureNamed(catalog, "llm-chat:chat.gguf",
        KiraService::Protocol::defaultModelOptions("llm-chat"), &error), qPrintable(error));
    auto routing = KiraService::Protocol::starterModelRouting();
    routing.presets[1].positivePrefix = "illustration";
    routing.presets[1].negativePrefix = "lowres";
    routing.presets[1].model.modelId = "image-generation:image.gguf";
    routing.editingQualities[0].model.modelId = "image-generation:image.gguf";
    QVERIFY2(catalog.configureRouting(routing, &error), qPrintable(error));
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = root;
    configuration.llamaCompletion = directory.filePath("completion");
    KiraServiceDaemon daemon(configuration);
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    QSignalSpy finished(&client, &KiraService::ServiceClient::imageJobFinished);
    client.connectToService();
    QTRY_VERIFY(client.modelRouting().has_value() && client.canSubmit(KiraService::ImageOperation::Generate, "image-generation:image.gguf"));
    KiraService::ImageGenerationRequest request;
    request.prompt = "  un chat roux  ";
    request.negativePrompt = "blurry";
    request.presetId = "illustration";
    request.width = 512; request.height = 512;
    const auto read = [&](const QString &name) {
        QFile file(directory.filePath(name));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    const auto argument = [&](const QString &name) {
        const auto args = read("sd-args").split('\n');
        return args.value(args.indexOf(name) + 1);
    };
    const auto takeResult = [&] {
        [&] { QTRY_COMPARE(finished.count(), 1); }();
        return qvariant_cast<KiraService::ImageJobResult>(finished.takeFirst().at(0));
    };
    QVERIFY(!client.submitGeneration(request).isEmpty());
    auto result = takeResult();
    QVERIFY2(result.succeeded, qPrintable(result.message));
    // The result carries the submitted prompts; prefixes reach sd-cli only.
    QCOMPARE(result.prompt, QString("un chat roux"));
    QCOMPARE(result.negativePrompt, QString("blurry"));
    QCOMPARE(argument("--prompt"), QString("illustration, un chat roux"));
    QCOMPARE(argument("--negative-prompt"), QString("lowres, blurry"));
    QVERIFY(!QFileInfo::exists(directory.filePath("llm-called")));
    request.qualityId = "edit-low";
    QVERIFY(client.submitGeneration(request).isEmpty()); // Generation is routed by preset only.
    KiraService::ImageGenerationRequest edit;
    edit.qualityId = "edit-low";
    edit.prompt = "make it blue";
    edit.width = 512; edit.height = 512;
    edit.editImages = {QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=")};
    QVERIFY(!client.submitEdit(edit).isEmpty()); // The SDK resolves the quality's model.
    result = takeResult();
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QVERIFY(read("sd-args").contains(QDir(root).filePath("image.gguf")));
    edit.qualityId = "edit-medium";
    QVERIFY(client.submitEdit(edit).isEmpty()); // Unassigned qualities cannot submit.
}

void ServiceTest::enhancesPromptThroughSdk()
{
    using namespace KiraService::Protocol;
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath("llm"));
    for (const auto& name : {"llm.gguf"}) {
        QFile model(directory.filePath(QString("llm/%1").arg(name)));
        QVERIFY(model.open(QIODevice::WriteOnly)); model.write("fixture");
    }
    ModelCatalog catalog(directory.path(), false);
    auto parameters = defaultModelOptions("llm-chat");
    QVERIFY(configureNamed(catalog, "llm-chat:llm.gguf", parameters));
    const auto executablePath = directory.filePath("completion");
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write(
        "#!/bin/sh\nroot=$(dirname \"$0\")\nprintf '%s\\n' \"$@\" > \"$root/args\"\n"
        "previous=\nfor argument in \"$@\"; do\n"
        "if [ \"$previous\" = --file ]; then cat \"$argument\" > \"$root/input\"; fi\n"
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
    // Without llama-server, prompt.enhance falls back to llama-completion.
    configuration.llamaServer = directory.filePath("missing-llama-server");
    configuration.promptEnhancementTimeoutMs = 400;
    KiraServiceDaemon daemon(configuration); QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    QSignalSpy finished(&client, &KiraService::ServiceClient::promptEnhancementFinished);
    QSignalSpy imageFinished(&client, &KiraService::ServiceClient::imageJobFinished);
    client.connectToService();
    QTRY_VERIFY(client.canEnhancePrompt("llm-chat:llm.gguf"));
    QVERIFY(client.enhancePrompt({"missing", "person", {}}).isEmpty());
    QVERIFY(!client.enhancePrompt({"llm-chat:llm.gguf", "  ", {}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
    QFile emptyInput(directory.filePath("input"));
    QVERIFY(emptyInput.open(QIODevice::ReadOnly));
    QVERIFY(emptyInput.readAll().contains("Generate a new description"));
    const QString prompt = "une personne\navec des cheveux bouclés; $(false)";
    auto id = client.enhancePrompt({"llm-chat:llm.gguf", prompt, {{"maxTokens", 80}, {"temperature", 0}, {"systemPrompt", "Keep the description in French."}}});
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
    // Top-k, min-p and the sampler order follow the model's GGUF recommendations or llama.cpp's defaults.
    QVERIFY(!arguments.contains("--top-k") && !arguments.contains("--min-p") && !arguments.contains("--samplers"));
    QVERIFY(arguments.contains("--single-turn\n"));
    QVERIFY(arguments.contains("--no-display-prompt\n"));
    // The instructions open the single user message, followed by the prompt.
    QCOMPARE(QString::fromUtf8(read("input")), "Keep the description in French.\n\n" + prompt);
    QVERIFY(!arguments.contains("--system-prompt-file"));
    const auto expectFailure = [&](const KiraService::PromptEnhancementRequest& request, const QString& code) {
        QVERIFY(!client.enhancePrompt(request).isEmpty());
        QTRY_COMPARE(finished.count(), 1);
        const auto failure = qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0));
        QVERIFY(!failure.succeeded); QCOMPARE(failure.errorCode, code); QVERIFY(failure.prompt.isEmpty());
    };
    expectFailure({"llm-chat:llm.gguf", "person", {{"unknownOption", true}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", "person", {{"temperature", "hot"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", "person", {{"penalizeRepeats", "no"}}}, "invalid-request");
    // Faithful rewrites may reuse the input's words: penalizeRepeats false lifts the penalty.
    QVERIFY(!client.enhancePrompt({"llm-chat:llm.gguf", "une personne", {{"penalizeRepeats", false}}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
    QVERIFY(read("args").contains("--repeat-penalty\n1.0\n--repeat-last-n\n0\n"));
    const auto marker = [&](const QString& name) { QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly); };
    QVERIFY(marker("fail"));
    expectFailure({"llm-chat:llm.gguf", "person", {}}, "prompt-enhancement-failed");
    QVERIFY(QFile::remove(directory.filePath("fail")));
    QVERIFY(marker("empty"));
    expectFailure({"llm-chat:llm.gguf", "person", {}}, "prompt-enhancement-empty-result");
    QVERIFY(QFile::remove(directory.filePath("empty")));
    QVERIFY(marker("large"));
    expectFailure({"llm-chat:llm.gguf", "person", {}}, "prompt-enhancement-output-too-large");
    QVERIFY(QFile::remove(directory.filePath("large")));
    QVERIFY(marker("wait"));
    expectFailure({"llm-chat:llm.gguf", "person", {}}, "prompt-enhancement-timeout");
    QVERIFY(QFile::remove(directory.filePath("wait")));
    QVERIFY(!client.enhancePrompt({"llm-chat:llm.gguf", "retry", {}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
    // Disconnect cancels active and queued work and releases the shared queue.
    QVERIFY(marker("wait"));
    QVERIFY(QFile::remove(directory.filePath("args")));
    QVERIFY(!client.enhancePrompt({"llm-chat:llm.gguf", "active", {}}).isEmpty());
    QVERIFY(!client.enhancePrompt({"llm-chat:llm.gguf", "queued", {}}).isEmpty());
    QTRY_VERIFY(QFileInfo::exists(directory.filePath("args")));
    client.disconnectFromService();
    QTRY_COMPARE(finished.count(), 2);
    for (const auto& notification : finished) {
        QVERIFY(!qvariant_cast<KiraService::PromptEnhancementResult>(notification.at(0)).succeeded);
    }
    finished.clear();
    QVERIFY(QFile::remove(directory.filePath("wait")));
    client.connectToService();
    QTRY_VERIFY(client.canEnhancePrompt("llm-chat:llm.gguf"));
    QVERIFY(!client.enhancePrompt({"llm-chat:llm.gguf", "after reconnect", {}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(finished.takeFirst().at(0)).succeeded);
}

void ServiceTest::chatsThroughCompletionEngine()
{
#ifdef Q_OS_WIN
    QSKIP("The fake llama-completion fixture requires a POSIX shell.");
#endif
    using namespace KiraService::Protocol;
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath("llm"));
    QFile model(directory.filePath("llm/llm.gguf"));
    QVERIFY(model.open(QIODevice::WriteOnly)); model.write("fixture"); model.close();
    ModelCatalog catalog(directory.path(), false);
    QVERIFY(configureNamed(catalog, "llm-chat:llm.gguf", defaultModelOptions("llm-chat")));
    const auto executablePath = directory.filePath("completion");
    QFile executable(executablePath);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write(
        "#!/bin/sh\nroot=$(dirname \"$0\")\n"
        "if [ \"$1\" = --help ]; then printf '  list of built-in templates:\\n  chatml, mistral-v7,\\n  zephyr\\n  (env: LLAMA_ARG_CHAT_TEMPLATE)\\n'; exit 0; fi\n"
        "printf '%s\\n' \"$@\" > \"$root/args\"\n"
        "previous=\nfor argument in \"$@\"; do\n"
        "if [ \"$previous\" = --file ]; then cat \"$argument\" > \"$root/input\"; fi\n"
        "if [ \"$previous\" = --json-schema-file ]; then cat \"$argument\" > \"$root/schema\"; fi\n"
        "previous=\"$argument\"\ndone\n"
        "if [ -f \"$root/wait\" ]; then exec sleep 30; fi\n"
        "if [ -f \"$root/fail\" ]; then printf 'backend failure' >&2; exit 9; fi\n"
        "if [ -f \"$root/thinking\" ]; then printf '<think>never closed'; exit 0; fi\n"
        "if [ -f \"$root/json\" ]; then printf ' {\"speech\":\"*smiles* Hi\"}\\n'; exit 0; fi\n"
        "if [ -f \"$root/badjson\" ]; then printf '{\"speech\":\"*smi'; exit 0; fi\n"
        "printf '<think>plan</think>\\n *smiles* Hello, Alex.\\nUser: I am fine.\\n [end of text]\\n'\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    configuration.llamaCompletion = executablePath;
    configuration.llamaServer = directory.filePath("missing-llama-server");
    configuration.promptEnhancementTimeoutMs = 400;
    KiraServiceDaemon daemon(configuration); QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    QSignalSpy finished(&client, &KiraService::ServiceClient::chatFinished);
    client.connectToService();
    QTRY_VERIFY(client.canChat("llm-chat:llm.gguf"));
    QCOMPARE(client.capabilities().textChatEngines, QStringList{"completion"});
    const QList<KiraService::TextChatMessage> conversation{
        {"system", "You are Mia."}, {"user", "Hi."}, {"assistant", "Hey."}, {"user", "*sits down* How are you?"}};
    const QJsonObject completion{{"engine", "completion"}};
    const auto read = [&](const QString& name) {
        QFile file(directory.filePath(name)); if (!file.open(QIODevice::ReadOnly)) return QByteArray(); return file.readAll();
    };
    const auto expectFailure = [&](const KiraService::TextChatRequest& request, const QString& code) {
        QVERIFY(!client.submitChat(request).isEmpty());
        QTRY_COMPARE(finished.count(), 1);
        const auto failure = qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0));
        QVERIFY(!failure.succeeded); QCOMPARE(failure.errorCode, code); QVERIFY(failure.text.isEmpty());
    };

    // The default engine is the managed llama-server, which is not available here.
    expectFailure({"llm-chat:llm.gguf", conversation, {}}, "backend-not-ready");
    // Malformed conversations and options fail before queueing.
    expectFailure({"llm-chat:llm.gguf", {{"user", "Hi."}, {"system", "Late system."}, {"user", "Hello?"}}, completion}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", {{"user", "Hi."}, {"assistant", "Hey."}}, completion}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", {{"narrator", "Hi."}}, completion}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", {{"user", "  "}}, completion}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"unknown", true}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"gpuLayers", "most"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"gpuLayers", 1000}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"kvCacheLocation", "disk"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"kvCacheType", "q2"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"stop", QJsonArray{""}}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"temperature", "hot"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"topK", 1.5}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"minP", 2}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"repeatPenalty", 0.5}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"seed", -2}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"flashAttention", "yes"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"jsonSchema", "object"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"jsonSchema", QJsonObject{{"type", "object"}}},
        {"stop", QJsonArray{"\nUser:"}}}}, "invalid-request");
    // The SDK refuses unlisted models and empty conversations without contacting the service.
    QVERIFY(client.submitChat({"llm-chat:missing.gguf", conversation, completion}).isEmpty());
    QVERIFY(client.submitChat({"llm-chat:llm.gguf", {}, completion}).isEmpty());

    // The conversation is flattened into one user turn; reasoning and text after a stop string are removed.
    auto id = client.submitChat({"llm-chat:llm.gguf", conversation, {{"engine", "completion"}, {"maxTokens", 200},
        {"stop", QJsonArray{"\nUser:"}}, {"gpuLayers", "auto"}, {"kvCacheLocation", "ram"}, {"kvCacheType", "q8_0"},
        {"topK", 40}, {"minP", 0.05}, {"repeatPenalty", 1.15}, {"seed", 7}, {"flashAttention", "on"}}});
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    auto result = qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0));
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QCOMPARE(result.jobId, id); QCOMPARE(result.text, QString("*smiles* Hello, Alex."));
    QCOMPARE(result.engine, QString("completion"));
    QVERIFY(result.finishReason.isEmpty()); QCOMPARE(result.promptTokens, -1);
    QVERIFY(result.durationMs >= 0); QVERIFY(result.coldStart);
    QCOMPARE(QString::fromUtf8(read("input")), QString("You are Mia.\n\nConversation so far:\nUser: Hi.\nAssistant: Hey.\n"
        "User: *sits down* How are you?\n\nWrite only the assistant's next reply, without a speaker label."));
    auto arguments = read("args");
    QVERIFY(arguments.contains("--n-predict\n200\n"));
    QVERIFY(arguments.contains("--n-gpu-layers\nauto\n--fit\non\n--no-kv-offload\n--cache-type-k\nq8_0\n--cache-type-v\nq8_0\n"
                               "--flash-attn\non\n"));
    QVERIFY(arguments.contains("--repeat-penalty\n1.15\n"));
    QVERIFY(arguments.contains("--samplers\npenalties;dry;top_k;top_p;min_p;xtc;temperature\n"));
    QVERIFY(arguments.contains("--reasoning\noff\n"));
    QVERIFY(arguments.contains("--dry-multiplier\n0\n--dry-base\n1.75\n--dry-allowed-length\n2\n--dry-penalty-last-n\n64\n"
                               "--xtc-probability\n0\n--xtc-threshold\n0.1\n"));
    QVERIFY(!arguments.contains("--chat-template"));
    QVERIFY(arguments.contains("--top-k\n40\n--min-p\n0.05\n--seed\n7\n"));

    // Defaults keep every layer and the KV cache on the GPU, and never let llama.cpp spill layers silently.
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", {{"user", "Hi."}}, completion}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0)).succeeded);
    arguments = read("args");
    QVERIFY(arguments.contains("--n-gpu-layers\nall\n--fit\noff\n--cache-type-k\nf16\n--cache-type-v\nf16\n--flash-attn\nauto\n"));
    QVERIFY(arguments.contains("--repeat-penalty\n1.1\n--repeat-last-n\n4096\n"));
    QVERIFY(arguments.contains("--top-k\n20\n--min-p\n0\n--seed\n-1\n"));
    QVERIFY(!arguments.contains("--no-kv-offload"));
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", {{"user", "Hi."}}, {{"engine", "completion"}, {"gpuLayers", 40}}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0)).succeeded);
    QVERIFY(read("args").contains("--n-gpu-layers\n40\n--fit\noff\n"));
    QVERIFY(!read("args").contains("--json-schema-file"));
    // Reasoning, DRY, XTC and a built-in chat template are passed through; unknown template names are refused.
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", {{"user", "Hi."}}, {{"engine", "completion"}, {"reasoning", "on"},
        {"dryMultiplier", 0.8}, {"dryBase", 1.5}, {"dryAllowedLength", 3}, {"dryPenaltyLastN", -1},
        {"xtcProbability", 0.5}, {"xtcThreshold", 0.2}, {"chatTemplate", "chatml"}}}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0)).succeeded);
    arguments = read("args");
    QVERIFY(arguments.contains("--reasoning\non\n"));
    QVERIFY(arguments.contains("--dry-multiplier\n0.8\n--dry-base\n1.5\n--dry-allowed-length\n3\n--dry-penalty-last-n\n-1\n"
                               "--xtc-probability\n0.5\n--xtc-threshold\n0.2\n"));
    QVERIFY(arguments.contains("--chat-template\nchatml\n"));
    expectFailure({"llm-chat:llm.gguf", {{"user", "Hi."}}, {{"engine", "completion"}, {"chatTemplate", "notatemplate"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", {{"user", "Hi."}}, {{"engine", "completion"}, {"reasoning", "maybe"}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", {{"user", "Hi."}}, {{"engine", "completion"}, {"dryMultiplier", -1}}}, "invalid-request");
    expectFailure({"llm-chat:llm.gguf", {{"user", "Hi."}}, {{"engine", "completion"}, {"xtcProbability", 2}}}, "invalid-request");

    const auto marker = [&](const QString& name) { QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly); };
    // A JSON schema constrains the reply, which must parse and is also returned decoded.
    const QJsonObject schema{{"type", "object"}, {"properties", QJsonObject{{"speech", QJsonObject{{"type", "string"}}}}},
        {"required", QJsonArray{"speech"}}};
    const QJsonObject structured{{"engine", "completion"}, {"jsonSchema", schema}};
    QVERIFY(marker("json"));
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", conversation, structured}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    result = qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0));
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QCOMPARE(result.text, QString("{\"speech\":\"*smiles* Hi\"}"));
    QCOMPARE(result.json.toObject().value("speech").toString(), QString("*smiles* Hi"));
    QVERIFY(read("args").contains("--json-schema-file\n"));
    QCOMPARE(QJsonDocument::fromJson(read("schema")).object(), schema);
    QVERIFY(QFile::remove(directory.filePath("json")));
    QVERIFY(marker("badjson"));
    expectFailure({"llm-chat:llm.gguf", conversation, structured}, "chat-invalid-json");
    QVERIFY(QFile::remove(directory.filePath("badjson")));
    // Without a schema the reply stays plain text.
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", conversation, completion}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0)).json.isUndefined());
    QVERIFY(marker("thinking"));
    expectFailure({"llm-chat:llm.gguf", conversation, completion}, "chat-empty-result");
    QVERIFY(QFile::remove(directory.filePath("thinking")));
    QVERIFY(marker("fail"));
    expectFailure({"llm-chat:llm.gguf", conversation, completion}, "chat-failed");
    QVERIFY(QFile::remove(directory.filePath("fail")));
    QVERIFY(marker("wait"));
    expectFailure({"llm-chat:llm.gguf", conversation, completion}, "chat-timeout");
    QVERIFY(QFile::remove(directory.filePath("wait")));
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", conversation, completion}).isEmpty());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0)).succeeded);
}

void ServiceTest::chatsThroughServerEngine()
{
#ifdef Q_OS_WIN
    QSKIP("The fake llama-completion fixture requires a POSIX shell.");
#endif
    using namespace KiraService::Protocol;
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath("llm"));
    QFile model(directory.filePath("llm/llm.gguf"));
    QVERIFY(model.open(QIODevice::WriteOnly)); model.write("fixture"); model.close();
    ModelCatalog catalog(directory.path(), false);
    QVERIFY(configureNamed(catalog, "llm-chat:llm.gguf", defaultModelOptions("llm-chat")));
    QFile completion(directory.filePath("completion"));
    QVERIFY(completion.open(QIODevice::WriteOnly));
    completion.write("#!/bin/sh\nprintf 'enhanced'\n");
    completion.close();
    QVERIFY(completion.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("KIRA_FAKE_LLAMA_DIR", directory.path().toUtf8());
    const auto unsetFakeDirectory = qScopeGuard([] { qunsetenv("KIRA_FAKE_LLAMA_DIR"); });
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    configuration.llamaCompletion = completion.fileName();
    configuration.llamaServer = QStringLiteral(KIRA_FAKE_LLAMA_SERVER);
    configuration.promptEnhancementTimeoutMs = 1500;
    configuration.llamaServerIdleMs = 1500;
    KiraServiceDaemon daemon(configuration); QString error;
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    QSignalSpy finished(&client, &KiraService::ServiceClient::chatFinished);
    QSignalSpy enhanced(&client, &KiraService::ServiceClient::promptEnhancementFinished);
    client.connectToService();
    QTRY_VERIFY(client.canChat("llm-chat:llm.gguf"));
    QCOMPARE(client.capabilities().textChatEngines, QStringList({"server", "completion"}));

    const auto read = [&](const QString& name) {
        QFile file(directory.filePath(name)); if (!file.open(QIODevice::ReadOnly)) return QByteArray(); return file.readAll();
    };
    const auto launches = [&] { return read("launches").count('\n'); };
    const auto serverAlive = [&] {
        const auto pid = read("pid");
        return !pid.isEmpty() && QFileInfo::exists("/proc/" + QString::fromUtf8(pid));
    };
    const auto marker = [&](const QString& name) { QFile file(directory.filePath(name)); return file.open(QIODevice::WriteOnly); };
    const QList<KiraService::TextChatMessage> conversation{
        {"system", "You are Mia."}, {"user", "Hi."}, {"assistant", "Hey."}, {"user", "*sits down* How are you?"}};
    const auto chat = [&](const QJsonObject& parameters) {
        KiraService::TextChatResult result;
        if (client.submitChat({"llm-chat:llm.gguf", conversation, parameters}).isEmpty()) return result;
        if (!QTest::qWaitFor([&] { return finished.count() == 1; }, 5000)) return result;
        return qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0));
    };
    const QJsonObject turn{{"maxTokens", 100}, {"stop", QJsonArray{"\nUser:"}}, {"kvCacheLocation", "ram"}, {"seed", 3}};

    // server is the default engine; the reply is cleaned and reports usage and timing.
    auto result = chat(turn);
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QCOMPARE(result.text, QString("*smiles* Hello")); QCOMPARE(result.engine, QString("server"));
    QCOMPARE(result.finishReason, QString("stop"));
    QCOMPARE(result.promptTokens, 42); QCOMPARE(result.completionTokens, 7);
    QVERIFY(result.coldStart); QVERIFY(result.durationMs >= 0);
    QCOMPARE(launches(), 1);
    const auto launch = read("launches");
    QVERIFY(launch.contains("--ctx-size 4096 --n-gpu-layers all --fit off --no-kv-offload --cache-type-k f16 --cache-type-v f16 "
                            "--flash-attn auto --reasoning off --host 127.0.0.1 --port "));
    QVERIFY(launch.contains(" --api-key-file ")); QVERIFY(!launch.contains("--api-key "));
    QVERIFY(launch.contains("--no-webui --parallel 1 --jinja"));
    const auto body = QJsonDocument::fromJson(read("request.json")).object();
    QCOMPARE(body.value("messages").toArray().size(), 4);
    QCOMPARE(body.value("messages").toArray().first().toObject().value("role").toString(), QString("system"));
    QCOMPARE(body.value("max_tokens").toInt(), 100); QCOMPARE(body.value("seed").toInt(), 3);
    QCOMPARE(body.value("top_k").toInt(), 20); QCOMPARE(body.value("min_p").toDouble(), 0.0);
    QCOMPARE(body.value("repeat_penalty").toDouble(), 1.1); QCOMPARE(body.value("repeat_last_n").toInt(), 4096);
    QCOMPARE(body.value("stop").toArray(), QJsonArray{"\nUser:"});
    QCOMPARE(body.value("samplers").toArray(), QJsonArray({"penalties", "dry", "top_k", "top_p", "min_p", "xtc", "temperature"}));
    QCOMPARE(body.value("dry_multiplier").toDouble(), 0.0); QCOMPARE(body.value("dry_base").toDouble(), 1.75);
    QCOMPARE(body.value("dry_allowed_length").toInt(), 2); QCOMPARE(body.value("dry_penalty_last_n").toInt(), 64);
    QCOMPARE(body.value("xtc_probability").toDouble(), 0.0); QCOMPARE(body.value("xtc_threshold").toDouble(), 0.1);
    QVERIFY(body.value("cache_prompt").toBool()); QVERIFY(!body.value("stream").toBool(true));

    // The loaded model is reused while the launch settings are unchanged, and restarted otherwise.
    result = chat(turn);
    QVERIFY(result.succeeded); QVERIFY(!result.coldStart); QCOMPARE(launches(), 1);
    auto changed = turn; changed.insert("kvCacheType", "q8_0");
    result = chat(changed);
    QVERIFY(result.succeeded); QVERIFY(result.coldStart); QCOMPARE(launches(), 2);

    // A JSON schema becomes llama-server's response_format; the reply is checked and returned decoded.
    const QJsonObject schema{{"type", "object"}, {"properties", QJsonObject{{"speech", QJsonObject{{"type", "string"}}}}},
        {"required", QJsonArray{"speech"}}};
    auto structured = changed; structured.remove("stop"); structured.insert("jsonSchema", schema);
    result = chat(structured);
    QVERIFY2(result.succeeded, qPrintable(result.message)); QVERIFY(!result.coldStart);
    QCOMPARE(result.json.toObject().value("speech").toString(), QString("*smiles* Hello"));
    const auto structuredBody = QJsonDocument::fromJson(read("request.json")).object();
    QCOMPARE(structuredBody.value("response_format").toObject(),
             QJsonObject({{"type", "json_schema"}, {"json_schema", QJsonObject{{"schema", schema}}}}));
    QVERIFY(!structuredBody.contains("stop"));
    QVERIFY(marker("truncated"));
    result = chat(structured);
    QCOMPARE(result.errorCode, QString("chat-invalid-json")); QVERIFY(result.message.contains("maxTokens"));
    QVERIFY(QFile::remove(directory.filePath("truncated")));

    // Reasoning and the chat template are launch settings: changing them restarts the server.
    auto templated = turn; templated.insert("kvCacheType", "q8_0");
    templated.insert("chatTemplate", "mistral-v7"); templated.insert("reasoning", "on"); templated.insert("dryMultiplier", 0.8);
    result = chat(templated);
    QVERIFY2(result.succeeded, qPrintable(result.message)); QVERIFY(result.coldStart); QCOMPARE(launches(), 3);
    QVERIFY(read("launches").contains("--flash-attn auto --chat-template mistral-v7 --reasoning on --host"));
    QCOMPARE(QJsonDocument::fromJson(read("request.json")).object().value("dry_multiplier").toDouble(), 0.8);
    templated.insert("chatTemplate", "notatemplate");
    result = chat(templated);
    QCOMPARE(result.errorCode, QString("invalid-request")); QCOMPARE(launches(), 3);

    // prompt.enhance runs on llama-server with text.chat's default load settings: a model loaded with
    // other settings restarts, then consecutive jobs and such chats share the loaded model.
    const auto enhance = [&](const QJsonObject& parameters) {
        KiraService::PromptEnhancementResult result;
        if (client.enhancePrompt({"llm-chat:llm.gguf", "person", parameters}).isEmpty()) return result;
        if (!QTest::qWaitFor([&] { return enhanced.count() == 1; }, 5000)) return result;
        return qvariant_cast<KiraService::PromptEnhancementResult>(enhanced.takeFirst().at(0));
    };
    QVERIFY(serverAlive());
    auto enhancement = enhance({});
    QVERIFY2(enhancement.succeeded, qPrintable(enhancement.message));
    QCOMPARE(enhancement.prompt, QString("*smiles* Hello\nUser: hi"));
    QCOMPARE(launches(), 4);
    QVERIFY(read("launches").split('\n').at(3).contains(
        "--ctx-size 4096 --n-gpu-layers all --fit off --cache-type-k f16 --cache-type-v f16 --flash-attn auto --reasoning off --host"));
    auto enhanceBody = QJsonDocument::fromJson(read("request.json")).object();
    const auto messages = enhanceBody.value("messages").toArray();
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().toObject().value("role").toString(), QString("user"));
    QVERIFY(messages.first().toObject().value("content").toString().endsWith("reasoning.\n\nperson"));
    QCOMPARE(enhanceBody.value("temperature").toDouble(), 0.8); QCOMPARE(enhanceBody.value("max_tokens").toInt(), 1024);
    QCOMPARE(enhanceBody.value("repeat_penalty").toDouble(), 1.1); QCOMPARE(enhanceBody.value("repeat_last_n").toInt(), 4096);
    QVERIFY(!enhanceBody.contains("top_k")); QVERIFY(!enhanceBody.contains("samplers")); QVERIFY(!enhanceBody.contains("seed"));
    enhancement = enhance({{"penalizeRepeats", false}});
    QVERIFY(enhancement.succeeded); QCOMPARE(launches(), 4);
    enhanceBody = QJsonDocument::fromJson(read("request.json")).object();
    QCOMPARE(enhanceBody.value("repeat_penalty").toDouble(), 1.0); QCOMPARE(enhanceBody.value("repeat_last_n").toInt(), 0);
    result = chat({{"maxTokens", 100}});
    QVERIFY(result.succeeded); QVERIFY(!result.coldStart); QCOMPARE(launches(), 4);

    // Any other GPU work unloads the chat model first.
    result = chat({{"engine", "completion"}});
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QVERIFY(!serverAlive());
    result = chat(turn);
    QVERIFY(result.succeeded); QVERIFY(result.coldStart); QCOMPARE(launches(), 5);

    // Errors map to explicit codes, and the next turn recovers.
    QVERIFY(marker("context"));
    result = chat(turn);
    QCOMPARE(result.errorCode, QString("chat-context-exceeded")); QVERIFY(result.message.contains("5000"));
    QVERIFY(QFile::remove(directory.filePath("context")));
    QVERIFY(marker("crash"));
    result = chat(turn);
    QCOMPARE(result.errorCode, QString("chat-failed"));
    QVERIFY(QFile::remove(directory.filePath("crash")));
    result = chat(turn);
    QVERIFY(result.succeeded); QVERIFY(result.coldStart);
    QVERIFY(marker("hang"));
    result = chat(turn);
    QCOMPARE(result.errorCode, QString("chat-timeout"));
    QVERIFY(QFile::remove(directory.filePath("hang")));
    QVERIFY(marker("load-fail"));
    result = chat(turn);
    QCOMPARE(result.errorCode, QString("chat-failed")); QVERIFY(result.message.contains("out of memory"));
    QVERIFY(QFile::remove(directory.filePath("load-fail")));

    // A slow load is awaited through /health; an exact layer count is passed through.
    QVERIFY(marker("slow-load"));
    auto layers = turn; layers.insert("gpuLayers", 10);
    result = chat(layers);
    QVERIFY2(result.succeeded, qPrintable(result.message)); QVERIFY(result.coldStart);
    QVERIFY(read("launches").contains("--n-gpu-layers 10 --fit off"));
    QVERIFY(QFile::remove(directory.filePath("slow-load")));

    // The model unloads after the idle period.
    QVERIFY(serverAlive());
    QTRY_VERIFY_WITH_TIMEOUT(!serverAlive(), 5000);

    // text.chat.load preloads the model with the chat's own settings; text.chat.unload frees it.
    QVERIFY(client.capabilities().chatModelControl);
    QSignalSpy modelFinished(&client, &KiraService::ServiceClient::chatModelFinished);
    const auto control = [&](const QString& jobId) {
        KiraService::ChatModelResult result;
        if (jobId.isEmpty() || !QTest::qWaitFor([&] { return modelFinished.count() == 1; }, 5000)) return result;
        return qvariant_cast<KiraService::ChatModelResult>(modelFinished.takeFirst().at(0));
    };
    const auto launchesBeforeLoad = launches();
    QFile::remove(directory.filePath("request.json"));
    auto loaded = control(client.loadChatModel("llm-chat:llm.gguf", turn));
    QVERIFY2(loaded.succeeded, qPrintable(loaded.message)); QVERIFY(loaded.load); QVERIFY(loaded.coldStart);
    QVERIFY(loaded.durationMs >= 0);
    QCOMPARE(launches(), launchesBeforeLoad + 1); QVERIFY(serverAlive());
    QVERIFY(!QFileInfo::exists(directory.filePath("request.json")));
    loaded = control(client.loadChatModel("llm-chat:llm.gguf", turn));
    QVERIFY(loaded.succeeded); QVERIFY(!loaded.coldStart); QCOMPARE(launches(), launchesBeforeLoad + 1);
    result = chat(turn);
    QVERIFY(result.succeeded); QVERIFY(!result.coldStart); QCOMPARE(launches(), launchesBeforeLoad + 1);
    auto unloaded = control(client.unloadChatModel());
    QVERIFY(unloaded.succeeded); QVERIFY(!unloaded.load); QVERIFY(unloaded.unloaded);
    QTRY_VERIFY(!serverAlive());
    unloaded = control(client.unloadChatModel());
    QVERIFY(unloaded.succeeded); QVERIFY(!unloaded.unloaded);
    auto completionLoad = turn; completionLoad.insert("engine", "completion");
    loaded = control(client.loadChatModel("llm-chat:llm.gguf", completionLoad));
    QVERIFY(!loaded.succeeded); QCOMPARE(loaded.errorCode, QString("invalid-request"));
    QVERIFY(marker("load-fail"));
    loaded = control(client.loadChatModel("llm-chat:llm.gguf", turn));
    QVERIFY(!loaded.succeeded); QCOMPARE(loaded.errorCode, QString("chat-failed"));
    QVERIFY(loaded.message.contains("out of memory"));
    QVERIFY(QFile::remove(directory.filePath("load-fail")));

    // Disconnecting cancels the active chat and kills the server.
    QVERIFY(marker("hang"));
    QFile::remove(directory.filePath("request.json"));
    QVERIFY(!client.submitChat({"llm-chat:llm.gguf", conversation, turn}).isEmpty());
    QTRY_VERIFY(QFileInfo::exists(directory.filePath("request.json")));
    client.disconnectFromService();
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0)).succeeded);
    QTRY_VERIFY(!serverAlive());
}

void ServiceTest::chatsWithImagesThroughVisionProjector()
{
#ifdef Q_OS_WIN
    QSKIP("The fake llama-completion fixture requires a POSIX shell.");
#endif
    using namespace KiraService::Protocol;
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath("llm")); QVERIFY(QDir(directory.path()).mkpath("companion"));
    for (const auto& name : {"llm/vision.gguf", "llm/plain.gguf", "companion/projector.gguf"}) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
    }
    ModelCatalog catalog(directory.path(), false);
    QString error;
    QVERIFY2(configureNamed(catalog, "companion:projector.gguf", {{"subtype", "mmproj"}}, &error), qPrintable(error));
    auto describer = defaultModelOptions("llm-chat");
    describer.insert("defaultImageDescriber", true);
    QVERIFY2(catalog.configure("llm-chat:vision.gguf", describer, &error, {}, QString("Vision"), QString("companion:projector.gguf")),
             qPrintable(error));
    QVERIFY(configureNamed(catalog, "llm-chat:plain.gguf", defaultModelOptions("llm-chat")));
    QFile completion(directory.filePath("completion"));
    QVERIFY(completion.open(QIODevice::WriteOnly));
    completion.write("#!/bin/sh\nprintf 'enhanced'\n");
    completion.close();
    QVERIFY(completion.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("KIRA_FAKE_LLAMA_DIR", directory.path().toUtf8());
    const auto unsetFakeDirectory = qScopeGuard([] { qunsetenv("KIRA_FAKE_LLAMA_DIR"); });
    KiraServiceConfiguration configuration;
    configuration.port = 0; configuration.modelRootPath = directory.path();
    configuration.llamaCompletion = completion.fileName();
    configuration.llamaServer = QStringLiteral(KIRA_FAKE_LLAMA_SERVER);
    configuration.promptEnhancementTimeoutMs = 1500;
    configuration.llamaServerIdleMs = 1500;
    KiraServiceDaemon daemon(configuration);
    QVERIFY2(daemon.start(&error), qPrintable(error));
    KiraService::ServiceClient client;
    client.setConfiguration(QUrl(QString("ws://127.0.0.1:%1").arg(daemon.serverPort())), {});
    QSignalSpy finished(&client, &KiraService::ServiceClient::chatFinished);
    QSignalSpy enhanced(&client, &KiraService::ServiceClient::promptEnhancementFinished);
    client.connectToService();
    QTRY_VERIFY(client.canChat("llm-chat:vision.gguf"));
    QVERIFY(client.capabilities().textChatImages);
    QCOMPARE(client.defaultImageDescriberModelId(), QString("llm-chat:vision.gguf"));
    QVERIFY(client.canChatWithImages("llm-chat:vision.gguf"));
    QVERIFY(!client.canChatWithImages("llm-chat:plain.gguf"));

    const auto read = [&](const QString& name) {
        QFile file(directory.filePath(name)); if (!file.open(QIODevice::ReadOnly)) return QByteArray(); return file.readAll();
    };
    const auto launches = [&] { return read("launches").count('\n'); };
    const auto chat = [&](const QString& modelId, const QList<QByteArray>& images, const QJsonObject& parameters = {}) {
        KiraService::TextChatResult result;
        KiraService::TextChatRequest request{modelId, {{"system", "Describe."}, {"user", "What is shown?"}}, parameters};
        request.images = images;
        if (client.submitChat(request).isEmpty()) return result;
        if (!QTest::qWaitFor([&] { return finished.count() == 1; }, 5000)) return result;
        return qvariant_cast<KiraService::TextChatResult>(finished.takeFirst().at(0));
    };
    const QByteArray png("\x89PNG\r\n\x1a\nfirst");
    const QByteArray jpeg("\xff\xd8\xff\xe0second");

    // The images are uploaded, then shown in order before the last user message's text; the
    // vision LLM loads with its projector.
    auto result = chat("llm-chat:vision.gguf", {png, jpeg});
    QVERIFY2(result.succeeded, qPrintable(result.message)); QVERIFY(result.coldStart);
    QVERIFY(read("launches").contains("--mmproj " + directory.filePath("companion/projector.gguf").toUtf8() + " --reasoning off"));
    const auto messages = QJsonDocument::fromJson(read("request.json")).object().value("messages").toArray();
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.first().toObject().value("content").toString(), QString("Describe."));
    const auto content = messages.last().toObject().value("content").toArray();
    QCOMPARE(content.size(), 3);
    QCOMPARE(content.at(0).toObject().value("type").toString(), QString("image_url"));
    QCOMPARE(content.at(0).toObject().value("image_url").toObject().value("url").toString(),
             "data:image/png;base64," + QString::fromLatin1(png.toBase64()));
    QCOMPARE(content.at(1).toObject().value("image_url").toObject().value("url").toString(),
             "data:image/jpeg;base64," + QString::fromLatin1(jpeg.toBase64()));
    QCOMPARE(content.at(2).toObject(), QJsonObject({{"type", "text"}, {"text", "What is shown?"}}));

    // Chats without images and prompt enhancements share the vision LLM's launch.
    result = chat("llm-chat:vision.gguf", {});
    QVERIFY(result.succeeded); QVERIFY(!result.coldStart); QCOMPARE(launches(), 1);
    QVERIFY(QJsonDocument::fromJson(read("request.json")).object().value("messages").toArray().last()
                .toObject().value("content").isString());
    QVERIFY(!client.enhancePrompt({"llm-chat:vision.gguf", "person", {}}).isEmpty());
    QTRY_COMPARE(enhanced.count(), 1);
    QVERIFY(qvariant_cast<KiraService::PromptEnhancementResult>(enhanced.takeFirst().at(0)).succeeded);
    QCOMPARE(launches(), 1);

    // Only PNG and JPEG images are accepted; the engine and the count are checked.
    result = chat("llm-chat:vision.gguf", {png, "not an image"});
    QCOMPARE(result.errorCode, QString("invalid-image"));
    result = chat("llm-chat:vision.gguf", {png}, {{"engine", "completion"}});
    QCOMPARE(result.errorCode, QString("invalid-request"));
    QList<QByteArray> tooMany;
    for (int i = 0; i <= KiraService::ServiceClient::MaximumChatImages; ++i) tooMany.append(png);
    QVERIFY(client.submitChat({"llm-chat:vision.gguf", {{"user", "Hi."}}, {}, tooMany}).isEmpty());
    QVERIFY(client.submitChat({"llm-chat:plain.gguf", {{"user", "Hi."}}, {}, {png}}).isEmpty());

    // An LLM without a projector loads without --mmproj.
    result = chat("llm-chat:plain.gguf", {});
    QVERIFY(result.succeeded); QVERIFY(result.coldStart);
    QVERIFY(!read("launches").split('\n').at(1).contains("--mmproj"));

    // The role is exclusive like the prompt roles.
    auto other = defaultModelOptions("llm-chat");
    other.insert("defaultImageDescriber", true);
    QVERIFY(client.configureModel("llm-chat:plain.gguf", other, {}, QString("Plain")));
    QTRY_COMPARE(client.defaultImageDescriberModelId(), QString("llm-chat:plain.gguf"));
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
    QVERIFY2(installNamed(catalog, request, &error), qPrintable(error));
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
    QVERIFY(!installNamed(restored, request, &error));
    request.companionUrls = {{"unknown", server.url().toString()}};
    QVERIFY(!installNamed(restored, request, &error));
    request.companionUrls = {{"clip-l", true}};
    QVERIFY(!installNamed(restored, request, &error));
    const auto options = KiraService::Protocol::defaultModelOptions("image-generation");
    QVERIFY(configureNamed(restored, request.existingModelId, options, &error));
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
    QVERIFY(installNamed(restored, request, &error));
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
    QVERIFY(installNamed(restored, request, &error));
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
    socket.sendTextMessage("{\"type\":\"models.install\",\"displayName\":\"Fixture model\",\"installId\":\"attach\",\"category\":\"image-generation\",\"id\":\"image-generation:existing.gguf\",\"modelType\":\"diffusion\",\"companionUrls\":{}}");
    QTRY_VERIFY(messages.size() >= 2);
    QCOMPARE(messages.first().value("type").toString(), QString("models.install.accepted"));
    QCOMPARE(messages.last().value("type").toString(), QString("models.install.completed"));
    QCOMPARE(messages.last().value("model").toObject().value("modelType").toString(), QString("diffusion"));
    for (const auto &message : messages)
        QVERIFY(!QJsonDocument(message).toJson().contains(directory.path().toUtf8()));
    ModelCatalog catalog(directory.path(), true);
    QCOMPARE(catalog.modelById("image-generation:existing.gguf")->modelType, QString("diffusion"));
    messages.clear();
    socket.sendTextMessage("{\"type\":\"models.install\",\"displayName\":\"Fixture model\",\"installId\":\"preserve-type\",\"category\":\"image-generation\",\"id\":\"image-generation:existing.gguf\"}");
    QTRY_VERIFY(messages.size() >= 2);
    QCOMPARE(messages.last().value("model").toObject().value("modelType").toString(), QString("diffusion"));
    messages.clear();
    socket.sendTextMessage("{\"type\":\"models.install\",\"displayName\":\"Fixture model\",\"installId\":\"bad\",\"category\":\"image-generation\",\"id\":\"image-generation:existing.gguf\",\"companionUrls\":true}");
    QTRY_VERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().value("code").toString(), QString("invalid-model-install"));
}

void ServiceTest::installsOptionalCompanions()
{
    TestHttpServer server(QByteArray("model-bytes")); QVERIFY(server.start());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    ModelCatalog catalog(directory.path(), true);
    QSignalSpy finished(&catalog, &ModelCatalog::installFinished);
    for (const ModelCategory category : {ModelCategory::ImageGeneration}) {
        const QString categoryName = ModelCatalog::categoryName(category);
        const QString primaryName = QStringLiteral("shared.gguf");
        const QString companionName = category == ModelCategory::LlmChat ? QStringLiteral("mmproj.gguf") : QStringLiteral("vae.safetensors");
        QString error;
        const int count = finished.count();
        QVERIFY(installNamed(catalog, {categoryName, category, server.url(primaryName), server.url(companionName)}, &error));
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
    QCOMPARE(pairedCategories, 1);
    socket.close();
    for (const ModelCategory category : {ModelCategory::ImageGeneration}) {
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
    QVERIFY(installNamed(catalog, {QStringLiteral("failed-pair"), ModelCategory::ImageGeneration,
        server.url(), server.url(QStringLiteral("missing.safetensors"))}, &error));
    QTRY_COMPARE(failed.count(), 1);
    const QString errorMessage = failed.first().at(1).toString();
    QVERIFY(errorMessage.contains(QStringLiteral("HTTP 404")));
    QVERIFY(errorMessage.contains(QStringLiteral("vae missing.safetensors")));
    QCOMPARE(finished.count(), 0);
    QVERIFY(catalog.models(ModelCategory::ImageGeneration).isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("model.gguf"))));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("model.gguf.part"))));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("companions/model.gguf/missing.safetensors.part"))));
}

void ServiceTest::acceptsExternalModelUrls()
{
    QTemporaryDir directory; QVERIFY(directory.isValid()); ModelCatalog catalog(directory.path(), true);
    QString error;
    const ModelInstallRequest request{QStringLiteral("external-1"), ModelCategory::LlmChat,
                                      QUrl(QStringLiteral("https://models.example.test/releases/chat-model.gguf")), {}};
    QVERIFY(installNamed(catalog, request, &error));
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
    QVERIFY(!installNamed(catalog, {QStringLiteral("bad"), ModelCategory::Upscaler, QUrl(QStringLiteral("http://127.0.0.1:1/owner/repo/resolve/main/model.bin")), {}}, &error));
    QVERIFY(!error.isEmpty());
    error.clear();
    for (const ModelCategory category : {ModelCategory::LlmChat, ModelCategory::Upscaler}) {
        QVERIFY(!installNamed(catalog, {QStringLiteral("wrong-companion"), category,
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
    QVERIFY(!installNamed(catalog, request, &error));
    request.allowLocalFiles = true;
    QSignalSpy completed(&catalog, &ModelCatalog::installFinished);
    QVERIFY2(installNamed(catalog, request, &error), qPrintable(error));
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
    QVERIFY2(installNamed(catalog, replacement, &error), qPrintable(error));
    QTRY_COMPARE(completed.count(), 2);
    QCOMPARE(catalog.modelById(id)->companionFiles.size(), 2);
    QVERIFY(QFileInfo::exists(storage.filePath(model->companionFiles.value("llm").toString())));
    request.installId = "duplicate-import";
    request.displayName = "  My second model  ";
    QVERIFY2(installNamed(catalog, request, &error), qPrintable(error));
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
    QVERIFY(!configureNamed(catalog, duplicate->id, options, &error, {}, QString()));
    QCOMPARE(catalog.modelById(duplicate->id)->name, QString("My second model"));
    QVERIFY(configureNamed(catalog, duplicate->id, options, &error, {}, QString("Renamed model")));
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
