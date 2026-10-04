#include "../src/manager/AddModelDialog.h"
#include "../src/manager/ManageModelsWindow.h"
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
#include <QFileDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <kira/service/ModelOptions.h>

class ManagerUiTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
    QWebSocketServer m_server{"manager-ui-test", QWebSocketServer::NonSecureMode};
    QWebSocket *m_peer = nullptr;
    QList<QJsonObject> m_messages;
    QJsonObject m_saved;
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
                                                     {"name", "Test model"},
                                                     {"fileName", "test.gguf"},
                                                     {"bytes", 10},
                                                     {"companionFileName", "companions/test.gguf/vae.gguf"},
                                                     {"companionRole", "vae"},
                                                     {"recommendedOptions", m_saved}}}}}}}});
    }
  private slots:
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
                                if (object.value("type") == "authenticate")
                                    send({{"type", "authenticated"}});
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
        auto* start = dialog.findChild<QPushButton*>("startDownload");
        QVERIFY(start->isEnabled());
        m_messages.clear();
        start->click();
        QTRY_VERIFY(!m_messages.isEmpty());
        const QUrl transferUrl(m_messages.last().value("url").toString());
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
        for (const auto &category : {"image-generation", "edition", "llm-chat", "upscaler", "ip-adapter", "ip_adapter", "clip_vision", "photomaker"})
        {
            widget.setOptions(category);
            QCOMPARE(widget.options(), KiraService::Protocol::defaultModelOptions(category));
            QVERIFY(widget.isValid());
            QVERIFY(!widget.explicitContent());
        }
        widget.setOptions("ip_adapter", {{"strength", 0.75}});
        QCOMPARE(widget.options(), QJsonObject({{"strength", 0.75}}));
        widget.setOptions("clip_vision");
        widget.findChild<QCheckBox *>("normalize_input")->setChecked(false);
        widget.findChild<QSpinBox *>("image_size")->setValue(336);
        QCOMPARE(widget.options(), QJsonObject({{"image_size", 336}, {"normalize_input", false}}));
        QVERIFY(widget.isValid());
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
        dialog.findChild<QLineEdit*>("clipLUrl")->setText("https://example.test/clip_l.safetensors");
        dialog.findChild<QLineEdit*>("clipGUrl")->setText("https://example.test/clip_g.safetensors");
        dialog.findChild<QLineEdit*>("textEncoderUrl")->setText("https://example.test/encoder.gguf");
        dialog.findChild<QLineEdit*>("mmprojUrl")->setText("https://example.test/mmproj.gguf");
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
        QCOMPARE(request.value("companionUrls").toObject().value("llm").toString(), QString("https://example.test/encoder.gguf"));
        QCOMPARE(request.value("companionUrls").toObject().value("mmproj").toString(), QString("https://example.test/mmproj.gguf"));
        QCOMPARE(request.value("modelType").toString(), QString("diffusion"));
        QCOMPARE(request.value("companionUrls").toObject().size(), 4);
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
        QCOMPARE(tree->model()->rowCount(root), 7);
        auto branch = tree->model()->index(2, 0, root);
        auto model = tree->model()->index(0, 0, branch);
        tree->setCurrentIndex(model);
        auto *form = window.findChild<ModelOptionsWidget *>();
        QVERIFY(form->isVisible());
        tree->setCurrentIndex(tree->model()->index(0, 0, model));
        QVERIFY(form->isVisible());
        auto *steps = form->findChild<QSpinBox *>("steps");
        steps->setValue(42);
        auto* explicitContent = form->findChild<QCheckBox*>("explicitContent");
        QVERIFY(explicitContent);
        QVERIFY(!explicitContent->isChecked());
        explicitContent->setChecked(true);
        auto *save = window.findChild<QPushButton *>("saveOptions");
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(!save->isEnabled());
        QTRY_VERIFY(!m_messages.isEmpty() && m_messages.last().value("type") == "models.configure");
        QCOMPARE(m_messages.last().value("recommendedOptions").toObject().value("steps").toInt(), 42);
        QVERIFY(m_messages.last().value("explicitContent").toBool());
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
        auto category = tree->model()->index(2, 0, root);
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
        auto category = tree->model()->index(2, 0, root);
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
        auto category = tree->model()->index(2, 0, root);
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
        auto category = tree->model()->index(2, 0, root);
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
        dialog.show();
        auto *category = dialog.findChild<QComboBox *>("modelCategory");
        auto *companion = dialog.findChild<QLineEdit *>("companionUrl");
        category->setCurrentIndex(category->findData("upscaler"));
        QVERIFY(!companion->isVisible());
        category->setCurrentIndex(category->findData("llm-chat"));
        QVERIFY(companion->isVisible());
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
