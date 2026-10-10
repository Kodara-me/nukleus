#include "../src/manager/AddModelDialog.h"
#include "../src/manager/ManageModelsWindow.h"
#include "../src/manager/ModelRoutingDialog.h"
#include <QStandardItemModel>
#include <QDoubleSpinBox>
#include <QAction>
#include "../src/manager/ServiceConnectionDialog.h"
#include "../src/manager/ManagerServiceClient.h"
#include "../src/manager/ModelOptionsWidget.h"
#include <KiraUi/Theme.h>
#include <KiraUi/TreeView.h>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QUuid>
#include <QtTest>
#include <kira/service/ModelOptions.h>
#include <algorithm>

class ManagerUiTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
    QWebSocketServer m_server{"manager-ui-test", QWebSocketServer::NonSecureMode};
    QWebSocket *m_peer = nullptr;
    QList<QJsonObject> m_messages;
    QJsonObject m_saved;
    QString m_requiredToken;
    void send(const QJsonObject &object)
    {
        m_peer->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    }
    void listModels()
    {
        send({{"type", "models.list.result"},
              {"categories",
               QJsonArray{QJsonObject{
                   {"category", "image-generation"},
                   {"models", QJsonArray{QJsonObject{{"id", "image-generation:test.gguf"},
                                                     {"name", "Test model"}, {"displayName", "Test model"},
                                                     {"fileName", "test.gguf"},
                                                     {"bytes", 10},
                                                     {"companionFileName", "companions/test.gguf/vae.gguf"},
                                                     {"companionRole", "vae"},
                                                     {"recommendedOptions", m_saved}}}}}}}});
    }
  private slots:
    void imageCapabilitiesAndSharedEncoderChoices()
    {
        ModelOptionsWidget form;
        ManagedModelInfo vae;
        vae.id = "companion:vae.gguf"; vae.displayName = "Shared VAE";
        vae.recommendedOptions = {{"subtype", "vae"}};
        ManagedModelInfo projector = vae;
        projector.id = "companion:vision.gguf";
        projector.recommendedOptions = {{"subtype", "mmproj"}};
        ManagedModelInfo encoder;
        encoder.id = "llm-chat:encoder.gguf"; encoder.displayName = "Encoder";
        encoder.recommendedOptions = {{"canBeTextEncoder", true}};
        ManagedModelInfo chat = encoder;
        chat.id = "llm-chat:chat.gguf"; chat.recommendedOptions = {{"canBeTextEncoder", false}};
        form.setModels({vae, projector, encoder, chat});
        form.setOptions("image-generation");
        auto *edit = form.findChild<QCheckBox*>("canEdit");
        auto *generate = form.findChild<QCheckBox*>("canGenerate");
        edit->setChecked(false); generate->setChecked(false);
        QVERIFY(!form.isValid());
        edit->setChecked(true); QVERIFY(form.isValid());
        auto *vaeChoice = form.findChild<QComboBox*>("vaeModelId");
        auto *encoderChoice = form.findChild<QComboBox*>("textEncoderModelId");
        QCOMPARE(vaeChoice->count(), 2); QCOMPARE(encoderChoice->count(), 2);
        vaeChoice->setCurrentIndex(1); encoderChoice->setCurrentIndex(1);
        QCOMPARE(form.options().value("vaeModelId").toString(), vae.id);
        QCOMPARE(form.options().value("textEncoderModelId").toString(), encoder.id);
        form.setModels({encoder, vae, chat, projector});
        QCOMPARE(encoderChoice->currentData().toString(), encoder.id);
        form.setOptions("llm-chat");
        auto *eligible = form.findChild<QCheckBox*>("canBeTextEncoder");
        QVERIFY(!eligible->isChecked()); eligible->setChecked(true);
        QVERIFY(form.options().value("canBeTextEncoder").toBool());
        for (const auto &role : {"defaultPromptEnhancer", "defaultPromptTranslator", "defaultImageDescriber"}) {
            auto *roleCheck = form.findChild<QCheckBox*>(role);
            QVERIFY(roleCheck && !roleCheck->isChecked()); roleCheck->setChecked(true);
            QVERIFY(form.options().value(role).toBool());
        }
        QVERIFY(KiraService::Protocol::validateModelOptions("llm-chat", form.options()));
    }
    void savesConnectionOnlyAfterAuthentication()
    {
        m_requiredToken = "test-key";
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        ServiceConnectionDialog dialog(&client);
        dialog.show();
        auto *host = dialog.findChild<QLineEdit *>("serviceHostInput");
        auto *token = dialog.findChild<QLineEdit *>("serviceTokenInput");
        auto *button = dialog.findChild<QPushButton *>("connectService");
        QCOMPARE(token->echoMode(), QLineEdit::Password);
        host->setText("ws://127.0.0.1:9384"); button->click();
        QVERIFY(m_messages.isEmpty()); QVERIFY(!QSettings().contains("Service/Host"));
        host->setText("127.0.0.2"); token->setText("failed-key"); button->click();
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(dialog.isVisible()); QVERIFY(!QSettings().contains("Service/Token"));
        host->setText("127.0.0.1"); button->click();
        QTRY_VERIFY(!m_messages.isEmpty());
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(dialog.isVisible()); QVERIFY(!QSettings().contains("Service/Token"));
        host->setText(" localhost "); token->setText("test-key"); button->click();
        QVERIFY(!QSettings().contains("Service/Token"));
        QTRY_VERIFY(client.isAuthenticated());
        QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(QSettings().value("Service/Host").toString(), QString("localhost"));
        QCOMPARE(QSettings().value("Service/Token").toString(), QString("test-key"));
        ManagerServiceClient restored(nullptr, m_server.serverPort());
        ManageModelsWindow reopened(nullptr, &restored);
        QCOMPARE(restored.host(), QString("localhost"));
        QCOMPARE(restored.token(), QString("test-key"));
        QTRY_VERIFY(restored.isAuthenticated());
        reopened.close();
        const auto auth = std::find_if(m_messages.cbegin(), m_messages.cend(), [](const auto &message) {
            return message.value("type") == "authenticate" && message.value("token") == "test-key";
        });
        QVERIFY(auth != m_messages.cend());
        QCOMPARE(auth->value("token").toString(), QString("test-key"));
        ServiceConnectionDialog canceled(&client);
        canceled.show();
        canceled.findChild<QLineEdit *>("serviceHostInput")->setText("192.168.1.1");
        canceled.findChild<QLineEdit *>("serviceTokenInput")->setText("discard");
        canceled.findChild<QPushButton *>("cancelConnection")->click();
        QCOMPARE(client.host(), QString("localhost"));
        QCOMPARE(QSettings().value("Service/Token").toString(), QString("test-key"));
    }
    void promptsWhenStartupCannotConnect()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        QVERIFY(client.setConnection("127.0.0.2", {}));
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(window.findChild<ServiceConnectionDialog *>());
        auto *dialog = window.findChild<ServiceConnectionDialog *>();
        dialog->findChild<QLineEdit *>("serviceHostInput")->setText("127.0.0.1");
        dialog->findChild<QPushButton *>("connectService")->click();
        QTRY_VERIFY(client.modelListAvailable());
        QTRY_VERIFY(!window.findChild<ServiceConnectionDialog *>());
        QCOMPARE(QSettings().value("Service/Host").toString(), QString("127.0.0.1"));
        window.close();
    }
    void editsRoutingAfterAcknowledgment()
    {
        m_saved.insert("steps", 37); m_saved.insert("cfgScale", 5.75);
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false); client.connectToService();
        QTRY_VERIFY(client.modelListAvailable());
        send({{"type", "capabilities"}, {"operations", QJsonArray{"model-routing"}}});
        auto routing = KiraService::Protocol::starterModelRouting();
        send({{"type", "model-routing.get.completed"}, {"routing", KiraService::Protocol::modelRoutingToJson(routing)}});
        QTRY_VERIFY(client.modelRouting().has_value());
        const auto originalLocale = QLocale();
        const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(originalLocale); });
        QLocale::setDefault(QLocale("fr_CA"));
        ModelRoutingDialog dialog(&client); dialog.show();
        auto *tree = dialog.findChild<KiraUi::TreeView *>("routingTree");
        QVERIFY(tree);
        QCOMPARE(tree->dragDropMode(), QAbstractItemView::NoDragDrop);
        QCOMPARE(tree->editTriggers(), QAbstractItemView::EditTriggers(QAbstractItemView::NoEditTriggers));
        QVERIFY(!dialog.findChild<QAction *>("addQuality"));
        QVERIFY(!dialog.findChild<QAction *>("removeRoutingItem"));
        auto *model = qobject_cast<QStandardItemModel *>(tree->model());
        auto *generation = model->item(0);
        auto *editing = model->item(1);
        QCOMPARE(generation->rowCount(), 2);
        auto *preset = generation->child(0);
        QCOMPARE(preset->rowCount(), 0); // Presets have no prompt flow children.
        QCOMPARE(preset->text(), QString("Photorealism"));
        QCOMPARE(editing->rowCount(), 3);
        QCOMPARE(editing->child(0)->text(), QString("Fast"));
        QCOMPARE(editing->child(1)->text(), QString("Balanced"));
        QCOMPARE(editing->child(2)->text(), QString("Quality"));
        QVERIFY(!(preset->flags() & Qt::ItemIsEditable));
        QVERIFY(!(preset->flags() & Qt::ItemIsDragEnabled));
        QVERIFY(!dialog.findChild<QCheckBox *>("qualityPermanent"));
        auto *positive = dialog.findChild<QPlainTextEdit *>("presetPositive");
        auto *negative = dialog.findChild<QPlainTextEdit *>("presetNegative");
        auto *assignment = dialog.findChild<QComboBox *>("routingModel");
        auto *steps = dialog.findChild<QSpinBox *>("routingSteps");
        auto *cfg = dialog.findChild<QDoubleSpinBox *>("routingCfg");
        auto *save = dialog.findChild<QPushButton *>("saveRouting");
        auto *identityViews = dialog.findChild<QComboBox *>("presetIdentityViews");
        QVERIFY(positive->isVisible()); QVERIFY(negative->isVisible()); QVERIFY(assignment->isVisible());
        QVERIFY(identityViews->isVisible());
        QCOMPARE(identityViews->currentData().toInt(), 1);
        identityViews->setCurrentIndex(identityViews->findData(4));
        positive->setPlainText("line one\nline two");
        QVERIFY(!steps->isEnabled()); QVERIFY(!cfg->isEnabled());
        QCOMPARE(assignment->currentData().toString(), QString());
        QVERIFY(assignment->count() > 1);
        assignment->setCurrentIndex(1);
        const auto assignedModelId = assignment->currentData().toString();
        QCOMPARE(steps->value(), 37);
        QCOMPARE(cfg->value(), 5.75);
        steps->setValue(4); cfg->setValue(1.0);
        tree->setCurrentIndex(generation->child(1)->index());
        assignment->setCurrentIndex(1);
        QCOMPARE(steps->value(), 37);
        QCOMPARE(cfg->value(), 5.75);
        m_saved.insert("steps", 88); m_saved.insert("cfgScale", 9.25);
        listModels();
        QTRY_COMPARE(client.models("image-generation").first().recommendedOptions.value("steps").toInt(), 88);
        QCOMPARE(steps->value(), 37); // Refresh never replaces the preset's snapshot.
        tree->setCurrentIndex(preset->index());
        QCOMPARE(steps->value(), 4); QCOMPARE(cfg->value(), 1.0);
        assignment->setCurrentIndex(0);
        assignment->setCurrentIndex(1);
        QCOMPARE(steps->value(), 88); QCOMPARE(cfg->value(), 9.25);
        steps->setValue(4); cfg->setValue(1.0);
        auto *defaultQuality = dialog.findChild<QCheckBox *>("editingDefaultQuality");
        QVERIFY(defaultQuality);
        tree->setCurrentIndex(editing->index());
        QVERIFY(!assignment->isVisible()); // The Editing group holds the qualities.
        tree->setCurrentIndex(generation->child(0)->index());
        QVERIFY(!defaultQuality->isVisible());
        tree->setCurrentIndex(editing->child(1)->index());
        QVERIFY(positive->isVisible()); QVERIFY(assignment->isVisible()); // Qualities have prefixes...
        QVERIFY(!identityViews->isVisible()); // ...but no names or identity views.
        QVERIFY(!dialog.findChild<QLineEdit *>("presetName")->isVisible());
        QVERIFY(positive->toPlainText().isEmpty());
        positive->setPlainText("sharp details");
        negative->setPlainText("blurry");
        QVERIFY(defaultQuality->isVisible());
        QVERIFY(defaultQuality->isChecked()); QVERIFY(!defaultQuality->isEnabled()); // Balanced starts as the default.
        assignment->setCurrentIndex(1);
        QCOMPARE(steps->value(), 88); QCOMPARE(cfg->value(), 9.25);
        steps->setValue(12); cfg->setValue(2.25);
        tree->setCurrentIndex(editing->child(2)->index());
        QVERIFY(!defaultQuality->isChecked()); QVERIFY(defaultQuality->isEnabled());
        defaultQuality->click(); // Moving the default clears it from Balanced.
        QVERIFY(!defaultQuality->isEnabled());
        tree->setCurrentIndex(editing->child(1)->index());
        QVERIFY(!defaultQuality->isChecked());
        QVERIFY(!dialog.findChild<QWidget *>("promptFlowPage"));
        if (qEnvironmentVariableIsSet("KIRA_MANAGER_SCREENSHOT_DIR")) {
            const QDir directory(qEnvironmentVariable("KIRA_MANAGER_SCREENSHOT_DIR"));
            QVERIFY(directory.mkpath("."));
            tree->setCurrentIndex(editing->index());
            QCoreApplication::processEvents();
            QVERIFY(dialog.grab().save(directory.filePath("routing-editing.png")));
            tree->setCurrentIndex(preset->index());
            QCoreApplication::processEvents();
            QVERIFY(dialog.grab().save(directory.filePath("routing-preset.png")));
        }
        QVERIFY(save->isEnabled()); save->click();
        QTRY_VERIFY(m_messages.last().value("type") == "model-routing.configure");
        const auto sent = m_messages.last().value("routing").toObject();
        QCOMPARE(sent.value("version").toInt(), 6);
        const auto presets = sent.value("presets").toArray();
        QCOMPARE(presets.size(), 2);
        const auto savedPreset = presets.first().toObject();
        QCOMPARE(savedPreset.value("id").toString(), routing.presets.first().id);
        QCOMPARE(savedPreset.value("nameFr").toString(), QString("Photoréalisme"));
        QCOMPARE(savedPreset.value("positivePrefix").toString(), QString("line one\nline two"));
        QVERIFY(!savedPreset.contains("qualities"));
        const auto parsed = KiraService::Protocol::modelRoutingFromJson(sent);
        QVERIFY(parsed);
        QVERIFY(!savedPreset.contains("promptFlow"));
        QCOMPARE(parsed->presets[0].identityViews, 4);
        QCOMPARE(parsed->presets[1].identityViews, 1);
        QCOMPARE(parsed->presets[0].model.modelId, assignedModelId);
        QCOMPARE(parsed->presets[0].model.steps, std::optional<int>(4));
        QCOMPARE(parsed->presets[0].model.cfgScale, std::optional<double>(1.0));
        QCOMPARE(parsed->presets[1].model.steps, std::optional<int>(37));
        QCOMPARE(parsed->presets[1].model.cfgScale, std::optional<double>(5.75));
        QCOMPARE(parsed->editingQualities[1].model.modelId, assignedModelId);
        QVERIFY(parsed->editingQualities[0].model.modelId.isEmpty());
        QCOMPARE(parsed->defaultEditingQuality()->id, QString("edit-high"));
        const auto balanced = sent.value("editingQualities").toArray()[1].toObject();
        QCOMPARE(parsed->editingQualities[1].positivePrefix, QString("sharp details"));
        QCOMPARE(parsed->editingQualities[1].negativePrefix, QString("blurry"));
        QVERIFY(parsed->editingQualities[2].positivePrefix.isEmpty());
        QCOMPARE(parsed->presets[0].positivePrefix, QString("line one\nline two")); // Kept apart from qualities.
        QCOMPARE(balanced.value("steps").toInt(), 12);
        QCOMPARE(balanced.value("cfgScale").toDouble(), 2.25);
        QVERIFY(dialog.isVisible()); QVERIFY(!save->isEnabled());
        send({{"type", "error"}, {"operation", "model-routing.configure"}, {"message", "Retry"}});
        QTRY_VERIFY(save->isEnabled());
        tree->setCurrentIndex(preset->index());
        QCOMPARE(positive->toPlainText(), QString("line one\nline two"));
        save->click();
        send({{"type", "model-routing.configure.completed"}, {"routing", sent}});
        QTRY_VERIFY(!dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        ModelRoutingDialog canceled(&client); canceled.show();
        const int count = m_messages.size();
        canceled.findChild<QPlainTextEdit *>("presetPositive")->setPlainText("Discard");
        QMetaObject::invokeMethod(&canceled, "reject");
        QCOMPARE(m_messages.size(), count);
        QCOMPARE(client.modelRouting()->presets.first().positivePrefix, QString("line one\nline two"));
    }
    void addsAndRemovesPresets()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false); client.connectToService();
        QTRY_VERIFY(client.modelListAvailable());
        send({{"type", "capabilities"}, {"operations", QJsonArray{"model-routing"}}});
        const auto routing = KiraService::Protocol::starterModelRouting();
        send({{"type", "model-routing.get.completed"}, {"routing", KiraService::Protocol::modelRoutingToJson(routing)}});
        QTRY_VERIFY(client.modelRouting().has_value());
        ModelRoutingDialog dialog(&client); dialog.show();
        auto *tree = dialog.findChild<KiraUi::TreeView *>("routingTree");
        auto *generation = qobject_cast<QStandardItemModel *>(tree->model())->item(0);
        auto *editing = qobject_cast<QStandardItemModel *>(tree->model())->item(1);
        auto *add = dialog.findChild<QAbstractButton *>("addPreset");
        auto *remove = dialog.findChild<QAbstractButton *>("removePreset");
        auto *name = dialog.findChild<QLineEdit *>("presetName");
        auto *nameFr = dialog.findChild<QLineEdit *>("presetNameFr");
        auto *save = dialog.findChild<QPushButton *>("saveRouting");
        QVERIFY(add && remove && name && nameFr && save);
        QVERIFY(add->isEnabled()); QVERIFY(remove->isEnabled());
        tree->setCurrentIndex(editing->index());
        QVERIFY(!remove->isEnabled()); // Editing is not a preset.
        add->click();
        QCOMPARE(generation->rowCount(), 3);
        auto *added = generation->child(2);
        QCOMPARE(tree->currentIndex(), added->index());
        QCOMPARE(added->rowCount(), 0);
        QCOMPARE(name->text(), QString("New preset"));
        QCOMPARE(nameFr->text(), QString("Nouveau préréglage"));
        name->selectAll(); QTest::keyClicks(name, "Portrait");
        QCOMPARE(added->text(), QString("Portrait"));
        nameFr->selectAll(); QTest::keyClick(nameFr, Qt::Key_Delete);
        QVERIFY(!save->isEnabled()); // Both names are required.
        QTest::keyClicks(nameFr, "Portrait FR");
        QVERIFY(save->isEnabled());
        tree->setCurrentIndex(generation->child(0)->index());
        remove->click();
        QCOMPARE(generation->rowCount(), 2);
        QCOMPARE(generation->child(0)->text(), QString("Illustration"));
        tree->setCurrentIndex(generation->child(0)->index());
        remove->click();
        QCOMPARE(generation->rowCount(), 1);
        QVERIFY(!remove->isEnabled()); // Generation keeps at least one preset.
        for (int i = 1; i < KiraService::Protocol::MaximumGenerationPresets; ++i) add->click();
        QVERIFY(!add->isEnabled());
        for (int i = 1; i < KiraService::Protocol::MaximumGenerationPresets; ++i) {
            tree->setCurrentIndex(generation->child(1)->index());
            remove->click();
        }
        save->click();
        QTRY_VERIFY(m_messages.last().value("type") == "model-routing.configure");
        const auto parsed = KiraService::Protocol::modelRoutingFromJson(m_messages.last().value("routing").toObject());
        QVERIFY(parsed);
        QCOMPARE(parsed->presets.size(), 1);
        QVERIFY(parsed->presets[0].id.startsWith("preset-"));
        QCOMPARE(parsed->presets[0].name, QString("Portrait"));
        QCOMPARE(parsed->presets[0].nameFr, QString("Portrait FR"));
        QCOMPARE(parsed->presets[0].identityViews, 1);
    }
    void initTestCase()
    {
        QCoreApplication::setOrganizationName("KiraManagerTests");
        QCoreApplication::setApplicationName("ManagerUiTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        KiraUi::Theme::apply(*qApp);
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QWebSocketServer::newConnection, this,
                [this]
                {
                    m_peer = m_server.nextPendingConnection();
                    connect(m_peer, &QWebSocket::disconnected, m_peer, &QObject::deleteLater);
                    connect(m_peer, &QWebSocket::textMessageReceived, this,
                            [this](const QString &text)
                            {
                                const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
                                m_messages.append(object);
                                if (object.value("type") == "authenticate") {
                                    if (!m_requiredToken.isEmpty() && object.value("token") != m_requiredToken) {
                                        send({{"type", "error"}, {"code", "authentication-failed"}, {"message", "Invalid key"}});
                                        m_peer->close();
                                    } else send({{"type", "authenticated"}});
                                }
                                else if (object.value("type") == "models.list")
                                    listModels();
                                else if (object.value("type") == "models.install")
                                    send({{"type", "models.install.accepted"},
                                          {"installId", object.value("installId")}});
                                else if (object.value("type") == "models.install.cancel")
                                    send({{"type", "models.install.canceled"},
                                          {"installId", object.value("installId")}});
                            });
                });
    }
    void init()
    {
        m_messages.clear();
        m_requiredToken.clear();
        m_saved = KiraService::Protocol::defaultModelOptions("image-generation");
        QSettings().clear();
    }
    void browsesLocalModel()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.connectToService();
        QTRY_VERIFY(client.modelListAvailable());
        QTemporaryDir source;
        const QString path = source.filePath("downloaded.gguf");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("local model");
        file.close();
        AddModelDialog dialog(&client, "image-generation");
        dialog.show();
        const bool disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QTimer::singleShot(0, &dialog, [&]
        {
            auto* picker = dialog.findChild<QFileDialog*>();
            QVERIFY(picker);
            picker->selectFile(path);
            QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        });
        dialog.findChild<QPushButton*>("modelUrlBrowse")->click();
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, disabled);
        QCOMPARE(QUrl(dialog.findChild<QLineEdit*>("modelUrl")->text()).toLocalFile(), path);
        dialog.findChild<QLineEdit*>("modelDisplayName")->setText("  My model  ");
        auto* start = dialog.findChild<QPushButton*>("startDownload");
        QVERIFY(start->isEnabled());
        m_messages.clear();
        start->click();
        QTRY_VERIFY(!m_messages.isEmpty());
        const QUrl transferUrl(m_messages.last().value("url").toString());
        QCOMPARE(m_messages.last().value("displayName").toString(), QString("My model"));
        QCOMPARE(transferUrl.host(), QString("127.0.0.1"));
        QCOMPARE(transferUrl.fileName(), QString("downloaded.gguf"));
        QNetworkAccessManager network;
        auto* reply = network.get(QNetworkRequest(transferUrl));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(reply->readAll(), QByteArray("local model"));
        reply->deleteLater();
        dialog.close();
        QTRY_VERIFY(!dialog.isVisible());
    }
    void fieldsFollowCategory()
    {
        ModelOptionsWidget widget;
        for (const auto &category : {"image-generation", "llm-chat", "upscaler", "companion"})
        {
            widget.setOptions(category);
            QCOMPARE(widget.options(), KiraService::Protocol::defaultModelOptions(category));
            QVERIFY(widget.isValid());
        QVERIFY(!widget.explicitContent());
        }
        widget.setOptions("llm-chat");
        QVERIFY(!widget.findChild<QComboBox *>("resolution"));
        widget.findChild<QSpinBox *>("maxTokens")->setValue(5000);
        QVERIFY(!widget.isValid());
        widget.setOptions("image-generation");
        auto *resolution = widget.findChild<QComboBox *>("resolution");
        QCOMPARE(resolution->count(), 8);
        resolution->setCurrentIndex(0);
        QCOMPARE(widget.options().value("width").toInt(), 256);
        resolution->setCurrentIndex(7);
        QCOMPARE(widget.options().value("height").toInt(), 2048);
    }

    void categoriesAreEmptyAndLlmSelectsSharedMmproj()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        client.connectToService();
        QTRY_VERIFY(client.modelListAvailable());
        const QJsonObject llm{{"id", "llm-chat:chat.gguf"}, {"fileName", "chat.gguf"}, {"displayName", "Chat"},
            {"recommendedOptions", KiraService::Protocol::defaultModelOptions("llm-chat")}};
        const QJsonObject vae{{"id", "companion:vae.safetensors"}, {"displayName", "VAE"},
            {"recommendedOptions", QJsonObject{{"subtype", "vae"}}}};
        const QJsonObject projector{{"id", "companion:vision.gguf"}, {"displayName", "Vision projector"},
            {"recommendedOptions", QJsonObject{{"subtype", "mmproj"}}}};
        const QJsonObject scaler{{"id", "upscaler:scale.gguf"}, {"fileName", "scale.gguf"}, {"displayName", "Scaler"}};
        const QJsonArray catalogue{
            QJsonObject{{"category", "llm-chat"}, {"models", QJsonArray{llm}}},
            QJsonObject{{"category", "companion"}, {"models", QJsonArray{vae, projector}}},
            QJsonObject{{"category", "upscaler"}, {"models", QJsonArray{scaler}}}};
        send({{"type", "models.list.result"}, {"categories", catalogue}});
        QTRY_COMPARE(client.models("companion").size(), 2);
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(client.modelListAvailable());
        send({{"type", "models.list.result"}, {"categories", catalogue}});
        QTRY_COMPARE(client.models("companion").size(), 2);
        auto* tree = window.findChild<KiraUi::TreeView*>("installedModelsTree");
        auto* body = window.findChild<QWidget*>("modelConfigurationBody");
        QVERIFY(body->isHidden());
        const auto select = [&](const QString& id) {
            const auto matches = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1, id, 1, Qt::MatchExactly | Qt::MatchRecursive);
            QVERIFY(!matches.isEmpty());
            tree->setCurrentIndex(matches.first());
        };
        select("llm-chat:chat.gguf");
        QVERIFY(!body->isHidden());
        auto* companion = window.findChild<QComboBox*>("modelMmprojCompanion");
        QCOMPARE(companion->count(), 2);
        QCOMPARE(companion->itemData(1).toString(), QString("companion:vision.gguf"));
        QVERIFY(!window.findChild<QLabel*>("recommendedValuesHint"));
        tree->setCurrentIndex(tree->model()->index(0, 0, tree->model()->index(0, 0)));
        QVERIFY(body->isHidden());
        select("upscaler:scale.gguf");
        auto* options = window.findChild<ModelOptionsWidget*>();
        QVERIFY(options->isHidden());
        QVERIFY(!options->findChild<QCheckBox*>("explicitContent"));
        QVERIFY(!options->findChild<QComboBox*>("tileSize"));
        QCOMPARE(options->options(), QJsonObject{});
        AddModelDialog add(&client, "llm-chat");
        add.show();
        QVERIFY(add.findChild<QLineEdit*>("companionUrl")->parentWidget()->isHidden());
        QVERIFY(!add.findChild<QLabel*>("recommendedValuesHint"));
        auto* selection = add.findChild<QComboBox*>("llmMmprojCompanion");
        QCOMPARE(selection->count(), 2);
        add.findChild<QLineEdit*>("modelUrl")->setText("https://example.com/chat.gguf");
        auto* start = add.findChild<QPushButton*>("startDownload");
        QVERIFY(!start->isEnabled());
        add.findChild<QLineEdit*>("modelDisplayName")->setText("New chat");
        QVERIFY(start->isEnabled());
        selection->setCurrentIndex(1);
        start->click();
        QTRY_COMPARE(m_messages.last().value("type").toString(), QString("models.install"));
        QCOMPARE(m_messages.last().value("displayName").toString(), QString("New chat"));
        QCOMPARE(m_messages.last().value("mmprojModelId").toString(), QString("companion:vision.gguf"));
        QVERIFY(!m_messages.last().contains("companionUrl"));
    }
    void attachesOptionalFilesToExistingModel()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.connectToService();
        QTRY_VERIFY(client.modelListAvailable());
        AddModelDialog dialog(&client, "image-generation", nullptr, "image-generation:test.gguf");
        dialog.show();
        QVERIFY(!dialog.findChild<QLineEdit*>("modelUrl")->isVisible());
        QVERIFY(!dialog.findChild<QLineEdit*>("companionUrl")->isEnabled());
        auto* remove = dialog.findChild<QPushButton*>("companionUrlRemove");
        QVERIFY(remove->isVisible());
        QTimer::singleShot(0, &dialog, [] {
            auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(question);
            question->button(QMessageBox::Yes)->click();
        });
        m_messages.clear();
        remove->click();
        QTRY_VERIFY(!m_messages.isEmpty());
        QCOMPARE(m_messages.last().value("type").toString(), QString("models.companion.delete"));
        QCOMPARE(m_messages.last().value("role").toString(), QString("vae"));
        send({{"type", "models.companion.delete.completed"}, {"id", "image-generation:test.gguf"}, {"role", "vae"}});
        QTRY_VERIFY(dialog.findChild<QLineEdit*>("companionUrl")->isEnabled());
        QVERIFY(!remove->isVisible());
        QVERIFY(!dialog.findChild<QLineEdit*>("clipLUrl"));
        QVERIFY(!dialog.findChild<QLineEdit*>("clipGUrl"));
        QVERIFY(!dialog.findChild<QLineEdit*>("textEncoderUrl"));
        QVERIFY(!dialog.findChild<QLineEdit*>("mmprojUrl"));
        dialog.findChild<QLineEdit*>("companionUrl")->setText("https://example.test/replacement.safetensors");
        dialog.findChild<QComboBox*>("modelType")->setCurrentIndex(1);
        if (qEnvironmentVariableIsSet("KIRA_MANAGER_SCREENSHOT_DIR")) {
            const QDir directory(qEnvironmentVariable("KIRA_MANAGER_SCREENSHOT_DIR"));
            QVERIFY(QDir().mkpath(directory.path()));
            QVERIFY(dialog.grab().save(directory.filePath("optional-model-files.png")));
        }
        auto* start = dialog.findChild<QPushButton*>("startDownload");
        QVERIFY(start->isEnabled());
        m_messages.clear();
        start->click();
        QTRY_VERIFY(!m_messages.isEmpty());
        const auto request = m_messages.last();
        QCOMPARE(request.value("id").toString(), QString("image-generation:test.gguf"));
        QCOMPARE(request.value("companionUrl").toString(), QString("https://example.test/replacement.safetensors"));
        QCOMPARE(request.value("modelType").toString(), QString("diffusion"));
        QVERIFY(request.value("companionUrls").toObject().isEmpty());
        QVERIFY(!request.contains("recommendedOptions"));
        QVERIFY(request.value("url").toString().isEmpty());
        QVERIFY(!start->isEnabled());
        dialog.close();
        QTRY_VERIFY(!dialog.isVisible());
    }
    void landscapeTreeAndSavedConfiguration()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(client.modelListAvailable());
        QCOMPARE(window.minimumSize(), QSize(1100, 640));
        QCOMPARE(window.maximumSize(), QSize(1100, 640));
        QCOMPARE(window.size(), QSize(1100, 640));
        window.resize(1000, 550);
        QCOMPARE(window.size(), QSize(1100, 640));
        window.resize(1500, 800);
        QCOMPARE(window.size(), QSize(1100, 640));
        auto *tree = window.findChild<KiraUi::TreeView *>("installedModelsTree");
        QVERIFY(tree);
        QCOMPARE(tree->model()->rowCount(), 1);
        auto root = tree->model()->index(0, 0);
        QCOMPARE(tree->model()->rowCount(root), 4);
        auto branch = tree->model()->index(1, 0, root);
        auto model = tree->model()->index(0, 0, branch);
        tree->setCurrentIndex(model);
        auto *form = window.findChild<ModelOptionsWidget *>();
        QVERIFY(form->isVisible());
        tree->setCurrentIndex(tree->model()->index(0, 0, model));
        QVERIFY(form->isVisible());
        auto *steps = form->findChild<QSpinBox *>("steps");
        auto* displayName = window.findChild<QLineEdit*>("modelDisplayName");
        QVERIFY(displayName);
        displayName->setText("Portrait model");
        steps->setValue(42);
        auto* explicitContent = form->findChild<QCheckBox*>("explicitContent");
        QVERIFY(explicitContent);
        QVERIFY(!explicitContent->isChecked());
        explicitContent->setChecked(true);
        QVERIFY(form->findChild<QCheckBox*>("canEdit")->isChecked());
        QVERIFY(form->findChild<QCheckBox*>("canGenerate")->isChecked());
        QVERIFY(!form->findChild<QComboBox*>("clipSkip"));
        auto *save = window.findChild<QPushButton *>("saveOptions");
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(!save->isEnabled());
        QTRY_VERIFY(!m_messages.isEmpty() && m_messages.last().value("type") == "models.configure");
        QCOMPARE(m_messages.last().value("recommendedOptions").toObject().value("steps").toInt(), 42);
        QCOMPARE(m_messages.last().value("displayName").toString(), QString("Portrait model"));
        QVERIFY(m_messages.last().value("explicitContent").toBool());
        QVERIFY(!m_messages.last().contains("canEdit"));
        send({{"type", "error"}, {"id", "image-generation:test.gguf"}, {"message", "Write failed"}});
        QTRY_VERIFY(save->isEnabled());
        QCOMPARE(steps->value(), 42);
        save->click();
        QTRY_VERIFY(!save->isEnabled());
        m_saved = form->options();
        send({{"type", "models.configure.completed"},
              {"id", "image-generation:test.gguf"},
              {"model", QJsonObject{{"recommendedOptions", m_saved}, {"explicitContent", true}}}});
        QTRY_VERIFY(form->isEnabled());
        QVERIFY(!save->isEnabled());
        client.requestModels();
        QTRY_VERIFY(tree->currentIndex().data(Qt::UserRole + 1).toString() == "image-generation:test.gguf");
        if (qEnvironmentVariableIsSet("KIRA_MANAGER_SCREENSHOT_DIR"))
        {
            const QString dir = qEnvironmentVariable("KIRA_MANAGER_SCREENSHOT_DIR");
            QVERIFY(window.grab().save(dir + "/manager-1100.png"));
        }
        window.close();
        QCOMPARE(QSettings().value("MainWindow/Geometry").toByteArray(), window.saveGeometry());
    }
    void warnsBeforeDiscardingEdits()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(client.modelListAvailable());
        auto *tree = window.findChild<KiraUi::TreeView *>();
        auto root = tree->model()->index(0, 0);
        auto category = tree->model()->index(1, 0, root);
        tree->setCurrentIndex(tree->model()->index(0, 0, category));
        window.findChild<QSpinBox *>("steps")->setValue(33);
        QTimer::singleShot(0,
                           []
                           {
                               auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                               QVERIFY(box);
                               box->button(QMessageBox::Cancel)->click();
                           });
        tree->setCurrentIndex(category);
        QCOMPARE(tree->currentIndex().data(Qt::UserRole + 1).toString(),
                 QString("image-generation:test.gguf"));
        QTimer::singleShot(0,
                           []
                           {
                               auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                               QVERIFY(box);
                               box->button(QMessageBox::Discard)->click();
                           });
        window.close();
        QVERIFY(!window.isVisible());
    }
    void handlesOlderServiceConfigureRejection()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(client.modelListAvailable());
        auto *tree = window.findChild<KiraUi::TreeView *>();
        auto root = tree->model()->index(0, 0);
        auto category = tree->model()->index(1, 0, root);
        tree->setCurrentIndex(tree->model()->index(0, 0, category));
        window.findChild<QSpinBox *>("steps")->setValue(35);
        auto *save = window.findChild<QPushButton *>("saveOptions");
        save->click();
        send({{"type", "error"}, {"code", "unsupported-message"}, {"message", "Unsupported message"}});
        QTRY_VERIFY(save->isEnabled());
        QTimer::singleShot(0,
                           []
                           {
                               auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                               QVERIFY(box);
                               box->button(QMessageBox::Discard)->click();
                           });
        window.close();
    }
    void savesBeforeChangingSelection()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(client.modelListAvailable());
        auto *tree = window.findChild<KiraUi::TreeView *>();
        auto root = tree->model()->index(0, 0);
        auto category = tree->model()->index(1, 0, root);
        tree->setCurrentIndex(tree->model()->index(0, 0, category));
        auto *form = window.findChild<ModelOptionsWidget *>();
        form->findChild<QSpinBox *>("steps")->setValue(34);
        QTimer::singleShot(0,
                           []
                           {
                               auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                               QVERIFY(box);
                               box->button(QMessageBox::Save)->click();
                           });
        tree->setCurrentIndex(category);
        QTRY_VERIFY(!m_messages.isEmpty() && m_messages.last().value("type") == "models.configure");
        QVERIFY(!tree->isEnabled());
        QVERIFY(form->isVisible());
        m_saved = form->options();
        send({{"type", "models.configure.completed"},
              {"id", "image-generation:test.gguf"},
              {"model", QJsonObject{{"recommendedOptions", m_saved}}}});
        QTRY_VERIFY(tree->isEnabled());
        QVERIFY(!form->isVisible());
        window.close();
    }
    void keepsEditsOnDisconnection()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        ManageModelsWindow window(nullptr, &client);
        window.show();
        QTRY_VERIFY(client.modelListAvailable());
        auto *tree = window.findChild<KiraUi::TreeView *>();
        auto root = tree->model()->index(0, 0);
        auto category = tree->model()->index(1, 0, root);
        tree->setCurrentIndex(tree->model()->index(0, 0, category));
        auto *form = window.findChild<ModelOptionsWidget *>();
        form->findChild<QSpinBox *>("steps")->setValue(45);
        window.findChild<QPushButton *>("saveOptions")->click();
        m_peer->close();
        QTRY_VERIFY(!client.isAuthenticated());
        QCOMPARE(form->options().value("steps").toInt(), 45);
        QTimer::singleShot(0,
                           []
                           {
                               auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                               QVERIFY(box);
                               box->button(QMessageBox::Discard)->click();
                           });
        window.close();
        QVERIFY(!window.isVisible());
    }
    void downloadModalRetriesCancelsAndClosesOnSuccess()
    {
        ManagerServiceClient client(nullptr, m_server.serverPort());
        client.setAutomaticReconnect(false);
        client.connectToService();
        QTRY_VERIFY(client.modelListAvailable());
        AddModelDialog dialog(&client);
        dialog.findChild<QLineEdit*>("modelDisplayName")->setText("Fixture model");
        dialog.show();
        auto *category = dialog.findChild<QComboBox *>("modelCategory");
        auto *companion = dialog.findChild<QLineEdit *>("companionUrl");
        category->setCurrentIndex(category->findData("upscaler"));
        QVERIFY(!companion->isVisible());
        category->setCurrentIndex(category->findData("llm-chat"));
        QVERIFY(!companion->isVisible());
        dialog.findChild<QLineEdit *>("modelUrl")->setText("https://example.test/model.gguf");
        auto *start = dialog.findChild<QPushButton *>("startDownload");
        QVERIFY(start->isEnabled());
        start->click();
        QTRY_VERIFY(client.installs().last().state == ManagedInstallState::Accepted);
        const QString first = client.installs().last().installId;
        send({{"type", "error"}, {"installId", first}, {"message", "Network failed"}});
        QTRY_VERIFY(start->isEnabled());
        QVERIFY(dialog.isVisible());
        start->click();
        QTRY_VERIFY(client.installs().last().state == ManagedInstallState::Accepted);
        dialog.close();
        QVERIFY(dialog.isVisible());
        QTRY_VERIFY(!dialog.isVisible());
        QCOMPARE(client.installs().last().state, ManagedInstallState::Canceled);
        AddModelDialog completed(&client);
        completed.findChild<QLineEdit*>("modelDisplayName")->setText("New model");
        completed.show();
        completed.findChild<QLineEdit *>("modelUrl")->setText("https://example.test/new.gguf");
        completed.findChild<QPushButton *>("startDownload")->click();
        QTRY_VERIFY(client.installs().last().state == ManagedInstallState::Accepted);
        send({{"type", "models.install.completed"},
              {"installId", client.installs().last().installId},
              {"model", QJsonObject{{"id", "image-generation:new.gguf"}}}});
        QTRY_VERIFY(!completed.isVisible());
        QCOMPARE(completed.result(), int(QDialog::Accepted));
        if (qEnvironmentVariableIsSet("KIRA_MANAGER_SCREENSHOT_DIR"))
        {
            AddModelDialog preview(&client);
            preview.show();
            QTest::qWait(20);
            QVERIFY(
                preview.grab().save(qEnvironmentVariable("KIRA_MANAGER_SCREENSHOT_DIR") + "/add-model.png"));
        }
    }
};
QTEST_MAIN(ManagerUiTest)
#include "ManagerUiTest.moc"
