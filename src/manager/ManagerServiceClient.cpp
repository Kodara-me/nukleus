#include "ManagerServiceClient.h"

#include <kira/service/ServiceProtocol.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QWebSocket>

#include <algorithm>

ManagerServiceClient::ManagerServiceClient(QObject *parent, quint16 localPort)
    : QObject(parent)
    , m_localPort(localPort)
    , m_socket(new QWebSocket(QStringLiteral("Kira Service Manager"), QWebSocketProtocol::VersionLatest, this))
    , m_retryTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(5000);
    connect(m_socket, &QWebSocket::connected, this, &ManagerServiceClient::authenticate);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &ManagerServiceClient::handleTextMessage);
    connect(m_socket, &QWebSocket::disconnected, this, [this] {
        m_authenticated = false;
        m_modelListAvailable = false;
        m_generationModels.clear(); m_editionModels.clear(); m_upscaleModels.clear(); m_llmChatModels.clear(); m_ipAdapterModels.clear();
        failActiveInstalls(tr("Service connection was closed"));
        emit modelsChanged();
        emit serviceStateChanged(QStringLiteral("offline"), tr("Service connection was closed"));
        if (m_automaticReconnect) m_retryTimer->start();
    });
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit serviceStateChanged(QStringLiteral("connection-error"), tr("Could not connect to the local Kira Service: %1").arg(m_socket->errorString()));
    });
    connect(m_retryTimer, &QTimer::timeout, this, &ManagerServiceClient::connectToService);
}

void ManagerServiceClient::connectToService()
{
    m_authenticated = false;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    emit serviceStateChanged(QStringLiteral("connecting"), tr("Connecting to the local Kira Service…"));
    m_socket->open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_localPort)));
}

void ManagerServiceClient::setAutomaticReconnect(bool enabled) { m_automaticReconnect = enabled; }
bool ManagerServiceClient::isAuthenticated() const { return m_authenticated; }
bool ManagerServiceClient::modelListAvailable() const { return m_modelListAvailable; }
QList<ManagedModelInfo> ManagerServiceClient::models(const QString &category) const
{
    if (category == QStringLiteral("image-generation")) return m_generationModels;
    if (category == QStringLiteral("edition")) return m_editionModels;
    if (category == QStringLiteral("upscaler")) return m_upscaleModels;
    if (category == QStringLiteral("llm-chat")) return m_llmChatModels;
    if (category == QStringLiteral("ip-adapter")) return m_ipAdapterModels;
    return {};
}
QList<ManagedInstall> ManagerServiceClient::installs() const { return m_installs; }
void ManagerServiceClient::requestModels() { if (m_authenticated) m_socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list\"}")); }

QString ManagerServiceClient::installModel(const QString &category, const QUrl &url, const QUrl &companionUrl, const QJsonObject &recommendedOptions)
{
    if (!m_authenticated) return {};
    const QString installId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_installs.append({installId, category, url, companionUrl, {}, ManagedInstallState::Requesting, 0, {}, {}, {}, {}});
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("models.install")}, {QStringLiteral("installId"), installId},
                        {QStringLiteral("category"), category}, {QStringLiteral("url"), url.toString()}};
    if (companionUrl.isValid() && !companionUrl.isEmpty()) {
        message.insert(QStringLiteral("companionUrl"), companionUrl.toString());
    }
    if (!recommendedOptions.isEmpty()) message.insert("recommendedOptions", recommendedOptions);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    emit installsChanged();
    return installId;
}

bool ManagerServiceClient::configureModel(const QString &id, const QJsonObject &options)
{
    if (!m_authenticated || id.isEmpty()) return false;
    const QJsonObject message{{"type", "models.configure"}, {"id", id}, {"recommendedOptions", options}};
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
        {QStringLiteral("version"), KiraService::Protocol::Version}, {QStringLiteral("token"), QString()}}).toJson(QJsonDocument::Compact)));
}

void ManagerServiceClient::handleTextMessage(const QString &message)
{
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8()); if (!document.isObject()) return;
    const QJsonObject object = document.object(); const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("authenticated")) { m_authenticated = true; requestModels(); emit serviceStateChanged(QStringLiteral("connected"), tr("Connected to service")); return; }
    if (type == QStringLiteral("models.list.result")) {
        m_generationModels.clear(); m_editionModels.clear(); m_upscaleModels.clear(); m_llmChatModels.clear(); m_ipAdapterModels.clear();
        for (const QJsonValue &categoryValue : object.value(QStringLiteral("categories")).toArray()) {
            const QJsonObject category = categoryValue.toObject(); QList<ManagedModelInfo> *modelsForCategory = nullptr;
            const QString name = category.value(QStringLiteral("category")).toString();
            if (name == QStringLiteral("image-generation")) modelsForCategory = &m_generationModels;
            else if (name == QStringLiteral("edition")) modelsForCategory = &m_editionModels;
            else if (name == QStringLiteral("upscaler")) modelsForCategory = &m_upscaleModels;
            else if (name == QStringLiteral("llm-chat")) modelsForCategory = &m_llmChatModels;
            else if (name == QStringLiteral("ip-adapter")) modelsForCategory = &m_ipAdapterModels;
            if (!modelsForCategory) continue;
            for (const QJsonValue &modelValue : category.value(QStringLiteral("models")).toArray()) {
                const QJsonObject model = modelValue.toObject();
                modelsForCategory->append({model.value(QStringLiteral("id")).toString(), model.value(QStringLiteral("name")).toString(),
                    model.value(QStringLiteral("fileName")).toString(), model.value(QStringLiteral("bytes")).toVariant().toLongLong(),
                    model.value(QStringLiteral("companionFileName")).toString(),
                                 model.value(QStringLiteral("companionRole")).toString(), model.value("recommendedOptions").toObject()});
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
        emit modelConfigured(object.value("id").toString(), object.value("model").toObject().value("recommendedOptions").toObject());
        requestModels();
        return;
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
