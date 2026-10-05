#include "ManagerServiceClient.h"

#include <kira/service/ServiceProtocol.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QWebSocket>
#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <memory>

#include <algorithm>

ManagerServiceClient::ManagerServiceClient(QObject *parent, quint16 localPort)
    : QObject(parent)
    , m_localPort(localPort)
    , m_socket(new QWebSocket(QStringLiteral("Kira Service Manager"), QWebSocketProtocol::VersionLatest, this))
    , m_retryTimer(new QTimer(this))
{
    connect(&m_localSourcesServer, &QTcpServer::newConnection, this, [this] {
        while (auto* socket = m_localSourcesServer.nextPendingConnection()) {
            auto file = std::make_shared<QFile>();
            auto header = std::make_shared<QByteArray>();
            auto started = std::make_shared<bool>(false);
            const auto sendChunk = [socket, file] {
                if (!file->isOpen()) return;
                while (socket->bytesToWrite() < 1024 * 1024 && !file->atEnd()) {
                    const auto bytes = file->read(256 * 1024);
                    if (bytes.isEmpty()) { socket->abort(); return; }
                    socket->write(bytes);
                }
                if (file->atEnd() && socket->bytesToWrite() == 0) {
                    file->close(); socket->disconnectFromHost();
                }
            };
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::bytesWritten, socket, [sendChunk](qint64) { sendChunk(); });
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, file, header, started, sendChunk] {
                if (*started) { socket->readAll(); return; }
                header->append(socket->readAll());
                if (header->size() > 16384) { socket->abort(); return; }
                if (!header->contains("\r\n\r\n")) return;
                *started = true;
                const auto parts = header->split(' ');
                const QString key = parts.size() >= 2 ? QUrl::fromEncoded(parts.at(1)).path(QUrl::FullyDecoded) : QString();
                file->setFileName(m_localSources.value(key));
                if (parts.first() != "GET" || file->fileName().isEmpty() || !file->open(QIODevice::ReadOnly)) {
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost(); return;
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(file->size())
                    + "\r\nConnection: close\r\n\r\n");
                sendChunk();
            });
        }
    });
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(5000);
    m_connectionTimeout.setSingleShot(true);
    m_connectionTimeout.setInterval(10000);
    connect(&m_connectionTimeout, &QTimer::timeout, this, [this] {
        m_socket->abort();
        emit serviceStateChanged("connection-error", tr("Service connection timed out"));
        if (m_automaticReconnect) m_retryTimer->start();
    });
    connect(m_socket, &QWebSocket::connected, this, &ManagerServiceClient::authenticate);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &ManagerServiceClient::handleTextMessage);
    connect(m_socket, &QWebSocket::disconnected, this, [this] {
        m_connectionTimeout.stop();
        m_authenticated = false;
        m_modelListAvailable = false;
        m_faceParsingModels.clear(); m_inpaintModels.clear(); m_generationModels.clear(); m_editionModels.clear(); m_upscaleModels.clear(); m_llmChatModels.clear(); m_ipAdapterModels.clear(); m_clipVisionModels.clear(); m_photoMakerModels.clear(); m_promptEnhancerModels.clear();
        failActiveInstalls(tr("Service connection was closed"));
        emit modelsChanged();
        if (!m_openingConnection) {
            emit serviceStateChanged(QStringLiteral("offline"), tr("Service connection was closed"));
            if (m_automaticReconnect) m_retryTimer->start();
        }
    });
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        m_connectionTimeout.stop();
        emit serviceStateChanged(QStringLiteral("connection-error"), tr("Could not connect to Kira Service: %1").arg(m_socket->errorString()));
        if (m_automaticReconnect) m_retryTimer->start();
    });
    connect(m_retryTimer, &QTimer::timeout, this, &ManagerServiceClient::connectToService);
}

void ManagerServiceClient::connectToService()
{
    m_retryTimer->stop();
    m_authenticated = false;
    m_openingConnection = true;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    m_openingConnection = false;
    emit serviceStateChanged(QStringLiteral("connecting"), tr("Connecting to Kira Service…"));
    QUrl endpoint;
    endpoint.setScheme("ws"); endpoint.setHost(m_host); endpoint.setPort(m_localPort);
    m_connectionTimeout.start();
    m_socket->open(endpoint);
}

bool ManagerServiceClient::setConnection(const QString &host, const QString &token)
{
    const QString normalized = host.trimmed();
    QHostAddress address;
    if (normalized != "localhost" && (!address.setAddress(normalized) || address.protocol() != QAbstractSocket::IPv4Protocol)) return false;
    m_host = normalized;
    m_token = token;
    return true;
}
QString ManagerServiceClient::host() const { return m_host; }
QString ManagerServiceClient::token() const { return m_token; }
void ManagerServiceClient::setAutomaticReconnect(bool enabled) { m_automaticReconnect = enabled; if (!enabled) m_retryTimer->stop(); }
bool ManagerServiceClient::isAuthenticated() const { return m_authenticated; }
bool ManagerServiceClient::modelListAvailable() const { return m_modelListAvailable; }
QList<ManagedModelInfo> ManagerServiceClient::models(const QString &category) const
{
    if (category == QStringLiteral("image-generation")) return m_generationModels;
    if (category == QStringLiteral("edition")) return m_editionModels;
    if (category == QStringLiteral("upscaler")) return m_upscaleModels;
    if (category == QStringLiteral("llm-chat")) return m_llmChatModels;
    if (category == QStringLiteral("ip-adapter") || category == "ip_adapter") return m_ipAdapterModels;
    if (category == "clip_vision") return m_clipVisionModels;
    if (category == "face-parsing") return m_faceParsingModels;
    if (category == "inpaint") return m_inpaintModels;
    if (category == "photomaker") return m_photoMakerModels;
    if (category == "prompt-enhancer") return m_promptEnhancerModels;
    return {};
}
QList<ManagedInstall> ManagerServiceClient::installs() const { return m_installs; }
void ManagerServiceClient::requestModels() { if (m_authenticated) m_socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list\"}")); }

QString ManagerServiceClient::installModel(const QString &category, const QUrl &url, const QUrl &companionUrl, const QJsonObject &recommendedOptions, const QJsonObject &companionUrls,
    const QString &modelType, const QString &existingModelId, std::optional<bool> explicitContent, std::optional<QString> displayName)
{
    if (!m_authenticated) return {};
    const QUrl preparedUrl = prepareSource(url);
    const QUrl preparedCompanion = prepareSource(companionUrl);
    if ((url.isLocalFile() && preparedUrl.isEmpty()) || (companionUrl.isLocalFile() && preparedCompanion.isEmpty())) return {};
    QJsonObject preparedFiles;
    for (auto it = companionUrls.begin(); it != companionUrls.end(); ++it) {
        const QUrl prepared = prepareSource(QUrl(it.value().toString()));
        if (prepared.isEmpty()) return {};
        preparedFiles.insert(it.key(), prepared.toString());
    }
    const QString installId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_installs.append({installId, category, url, companionUrl, {}, ManagedInstallState::Requesting, 0, {}, {}, {}, {}});
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("models.install")}, {QStringLiteral("installId"), installId},
                        {QStringLiteral("category"), category}, {QStringLiteral("url"), preparedUrl.toString()}};
    if (companionUrl.isValid() && !companionUrl.isEmpty()) {
        message.insert(QStringLiteral("companionUrl"), preparedCompanion.toString());
    }
    if (!recommendedOptions.isEmpty()) message.insert("recommendedOptions", recommendedOptions);
    if (!preparedFiles.isEmpty()) message.insert("companionUrls", preparedFiles);
    if (modelType != "checkpoint" || !existingModelId.isEmpty()) message.insert("modelType", modelType);
    if (!existingModelId.isEmpty()) message.insert("id", existingModelId);
    if (explicitContent) message.insert("explicitContent", *explicitContent);
    if (displayName) message.insert("displayName", displayName->trimmed());
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    emit installsChanged();
    return installId;
}

bool ManagerServiceClient::configureModelStyles(const QString &id, const QList<KiraService::PromptStyle> &styles)
{
    if (!m_authenticated || id.isEmpty() || !KiraService::Protocol::validatePromptStyles(styles)) return false;
    const QJsonObject message{{"type", "models.styles.configure"}, {"id", id},
        {"styles", KiraService::Protocol::promptStylesToJson(styles)}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}

bool ManagerServiceClient::configureModel(const QString &id, const QJsonObject &options, std::optional<bool> explicitContent, std::optional<QString> displayName)
{
    if (!m_authenticated || id.isEmpty()) return false;
    QJsonObject message{{"type", "models.configure"}, {"id", id}, {"recommendedOptions", options}};
    if (displayName) message.insert("displayName", displayName->trimmed());
    if (explicitContent) message.insert("explicitContent", *explicitContent);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}
bool ManagerServiceClient::cancelInstall(const QString &installId)
{
    if (!m_authenticated || installId.isEmpty()) return false;
    const QJsonObject message{{"type", "models.install.cancel"}, {"installId", installId}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}

bool ManagerServiceClient::deleteModel(const QString &id)
{
    if (!m_authenticated || id.isEmpty()) return false;
    const QJsonObject message{{QStringLiteral("type"), QStringLiteral("models.delete")}, {QStringLiteral("id"), id}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}

void ManagerServiceClient::authenticate()
{
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument({{QStringLiteral("type"), QStringLiteral("authenticate")},
        {QStringLiteral("version"), KiraService::Protocol::Version}, {QStringLiteral("token"), m_token}}).toJson(QJsonDocument::Compact)));
}

void ManagerServiceClient::handleTextMessage(const QString &message)
{
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8()); if (!document.isObject()) return;
    const QJsonObject object = document.object(); const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("authenticated")) { m_connectionTimeout.stop(); m_authenticated = true; requestModels(); emit serviceStateChanged(QStringLiteral("connected"), tr("Connected to service")); return; }
    if (type == QStringLiteral("models.list.result")) {
        m_faceParsingModels.clear(); m_inpaintModels.clear(); m_generationModels.clear(); m_editionModels.clear(); m_upscaleModels.clear(); m_llmChatModels.clear(); m_ipAdapterModels.clear(); m_clipVisionModels.clear(); m_photoMakerModels.clear(); m_promptEnhancerModels.clear();
        for (const QJsonValue &categoryValue : object.value(QStringLiteral("categories")).toArray()) {
            const QJsonObject category = categoryValue.toObject(); QList<ManagedModelInfo> *modelsForCategory = nullptr;
            const QString name = category.value(QStringLiteral("category")).toString();
            if (name == QStringLiteral("image-generation")) modelsForCategory = &m_generationModels;
            else if (name == QStringLiteral("edition")) modelsForCategory = &m_editionModels;
            else if (name == QStringLiteral("upscaler")) modelsForCategory = &m_upscaleModels;
            else if (name == QStringLiteral("llm-chat")) modelsForCategory = &m_llmChatModels;
            else if (name == QStringLiteral("ip-adapter") || name == "ip_adapter") modelsForCategory = &m_ipAdapterModels;
            else if (name == "clip_vision") modelsForCategory = &m_clipVisionModels;
            else if (name == "face-parsing") modelsForCategory = &m_faceParsingModels;
            else if (name == "inpaint") modelsForCategory = &m_inpaintModels;
            else if (name == "photomaker") modelsForCategory = &m_photoMakerModels;
            else if (name == "prompt-enhancer") modelsForCategory = &m_promptEnhancerModels;
            if (!modelsForCategory) continue;
            for (const QJsonValue &modelValue : category.value(QStringLiteral("models")).toArray()) {
                const QJsonObject model = modelValue.toObject();
                modelsForCategory->append({model.value(QStringLiteral("id")).toString(), model.value(QStringLiteral("name")).toString(),
                    model.value(QStringLiteral("fileName")).toString(), model.value(QStringLiteral("bytes")).toVariant().toLongLong(),
                    model.value(QStringLiteral("companionFileName")).toString(),
                                 model.value(QStringLiteral("companionRole")).toString(), model.value("recommendedOptions").toObject(),
                    model.value("companionFiles").toObject(), model.value("modelType").toString("checkpoint"), model.value("explicitContent").toBool(false),
                    KiraService::Protocol::promptStylesFromJson(model.value("styles")).value_or(QList<KiraService::PromptStyle>{}), model.value("displayName").toString().trimmed()});
            }
        }
        m_modelListAvailable = true; emit modelsChanged(); return;
    }
    if (type == QStringLiteral("models.install.accepted")) { if (auto *install = findInstall(object.value(QStringLiteral("installId")).toString())) { install->state = ManagedInstallState::Accepted; emit installsChanged(); } return; }
    if (type == QStringLiteral("models.install.progress")) { if (auto *install = findInstall(object.value(QStringLiteral("installId")).toString())) {
        install->fileRole = object.value("fileRole").toString();
        const QString state = object.value(QStringLiteral("state")).toString();
        install->state = state == QStringLiteral("queued") ? ManagedInstallState::Queued : state == QStringLiteral("verifying") ? ManagedInstallState::Verifying : ManagedInstallState::Downloading;
        install->bytesDownloaded = object.value(QStringLiteral("bytesDownloaded")).toVariant().toLongLong();
        install->totalBytes = object.contains(QStringLiteral("totalBytes")) ? std::optional<qint64>(object.value(QStringLiteral("totalBytes")).toVariant().toLongLong()) : std::nullopt;
        emit installsChanged(); } return; }
    if (type == QStringLiteral("models.install.completed")) { if (auto *install = findInstall(object.value(QStringLiteral("installId")).toString())) { install->modelId = object.value("model").toObject().value("id").toString(); install->companionFileName = object.value(QStringLiteral("model")).toObject().value(QStringLiteral("companionFileName")).toString(); install->state = ManagedInstallState::Completed; install->message = tr("Installed"); emit installsChanged(); } requestModels(); return; }
    if (type == "models.install.canceled") {
        if (auto *install = findInstall(object.value("installId").toString())) { install->state = ManagedInstallState::Canceled; emit installsChanged(); }
        return;
    }
    if (type == "models.configure.completed") {
        emit modelConfigured(object.value("id").toString(), object.value("model").toObject().value("recommendedOptions").toObject(), object.value("model").toObject().value("explicitContent").toBool(false));
        requestModels();
        return;
    }
    if (type == "models.styles.configure.completed") {
        const auto styles = KiraService::Protocol::promptStylesFromJson(object.value("styles"));
        if (styles) emit modelStylesConfigured(object.value("id").toString(), *styles);
        return;
    }
    if (type == "models.styles.changed") { requestModels(); return; }
    if (type == "models.companion.delete.completed") {
        emit companionDeleted(object.value("id").toString(), object.value("role").toString());
        requestModels(); return;
    }
    if (type == QStringLiteral("models.delete.completed")) { emit modelDeleted(object.value(QStringLiteral("id")).toString()); requestModels(); return; }
    if (type == QStringLiteral("error")) {
        const QString installId = object.value(QStringLiteral("installId")).toString();
        if (auto *install = findInstall(installId)) {
            if (install->state == ManagedInstallState::Completed || install->state == ManagedInstallState::Canceled) return;
            install->state = ManagedInstallState::Failed; install->message = object.value(QStringLiteral("message")).toString(); emit installsChanged(); return; }
        const QString id = object.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) { emit modelOperationFailed(id, object.value(QStringLiteral("message")).toString()); return; }
        if (!m_authenticated) { emit serviceStateChanged(QStringLiteral("authentication-failed"), object.value(QStringLiteral("message")).toString()); m_socket->close(); }
        else {
            const QString detail = object.value(QStringLiteral("message")).toString();
            failActiveInstalls(detail);
            emit serviceStateChanged(QStringLiteral("operation-error"), detail);
        }
    }
}

ManagedInstall *ManagerServiceClient::findInstall(const QString &installId)
{
    const auto it = std::find_if(m_installs.begin(), m_installs.end(), [&installId](const ManagedInstall &install) { return install.installId == installId; });
    return it == m_installs.end() ? nullptr : &*it;
}

void ManagerServiceClient::failActiveInstalls(const QString &message)
{
    bool changed = false;
    for (ManagedInstall &install : m_installs) if (install.state != ManagedInstallState::Completed && install.state != ManagedInstallState::Failed && install.state != ManagedInstallState::Canceled) { install.state = ManagedInstallState::Failed; install.message = message; changed = true; }
    if (changed) emit installsChanged();
}

QUrl ManagerServiceClient::prepareSource(const QUrl& source)
{
    if (!source.isLocalFile()) return source;
    const QFileInfo file(source.toLocalFile());
    if (!file.isFile() || !file.isReadable()) return {};
    if (!m_localSourcesServer.isListening() && !m_localSourcesServer.listen(QHostAddress::LocalHost)) return {};
    const QString key = "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "/" + file.fileName();
    m_localSources.insert(key, file.absoluteFilePath());
    QUrl url;
    url.setScheme("http"); url.setHost("127.0.0.1");
    url.setPort(m_localSourcesServer.serverPort()); url.setPath(key);
    return url;
}

bool ManagerServiceClient::deleteCompanion(const QString& id, const QString& role)
{
    if (!m_authenticated || id.isEmpty() || role.isEmpty()) return false;
    const QJsonObject message{{"type", "models.companion.delete"}, {"id", id}, {"role", role}};
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return true;
}
