#include "KiraService.h"
#include "ModelCatalog.h"
#include "ServicePaths.h"
#include "ServiceProtocol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
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
                socket->readAll();
                const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(m_payload.size()) + "\r\nConnection: close\r\n\r\n" + m_payload;
                socket->write(response); socket->disconnectFromHost();
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/owner/repo/resolve/main/model.gguf").arg(m_server.serverPort())); }

private:
    QTcpServer m_server;
    QByteArray m_payload;
};
}

class ServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesProtocolV3();
    void rejectsLanWithoutToken();
    void startsOnLoopbackAndLanAddress();
    void authenticatesLoopbackWithoutToken();
    void rejectsLanAuthenticationWithoutToken();
    void rollsBackWhenLanListenerFails();
    void listsSupportedModelsWithoutPathLeak();
    void removesModelByOpaqueId();
    void installsModelAtomically();
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

void ServiceTest::preservesProtocolV3()
{
    QCOMPARE(ServiceProtocol::Version, 3);
    QCOMPARE(ServiceProtocol::DefaultPort, quint16(9384));
}

void ServiceTest::rejectsLanWithoutToken()
{
    const QHostAddress lan = availableLanAddress();
    if (lan.isNull()) QSKIP("No non-loopback IPv4 address is available");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = lan; configuration.port = 0; configuration.modelRootPath = directory.path();
    KiraService service(configuration); QString error;
    QVERIFY(!service.start(&error)); QVERIFY(error.contains(QStringLiteral("token"), Qt::CaseInsensitive));
}

void ServiceTest::startsOnLoopbackAndLanAddress()
{
    const QHostAddress lan = availableLanAddress();
    if (lan.isNull()) QSKIP("No non-loopback IPv4 address is available");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = lan; configuration.port = 0; configuration.token = QStringLiteral("lan-secret"); configuration.modelRootPath = directory.path();
    KiraService service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
    QTcpSocket local; local.connectToHost(QHostAddress(QStringLiteral("127.0.0.1")), service.serverPort()); QVERIFY(local.waitForConnected(1000));
    QTcpSocket network; network.connectToHost(lan, service.serverPort()); QVERIFY(network.waitForConnected(1000));
}

void ServiceTest::authenticatesLoopbackWithoutToken()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    KiraServiceConfiguration configuration; configuration.listenAddress = QHostAddress(QStringLiteral("127.0.0.1")); configuration.port = 0; configuration.token = QStringLiteral("lan-secret"); configuration.modelRootPath = directory.path();
    KiraService service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
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
    KiraService service(configuration); QString error; QVERIFY2(service.start(&error), qPrintable(error));
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
    KiraService service(configuration); QString error; QVERIFY(!service.start(&error));
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
