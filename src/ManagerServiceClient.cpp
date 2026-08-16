#include "ManagerServiceClient.h"

#include "ServiceProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>
#include <QWebSocket>

#include <algorithm>

namespace
{
const QString EndpointKey = QStringLiteral("Manager/Endpoint");
const QString TokenKey = QStringLiteral("Manager/Token");
}

ManagerServiceClient::ManagerServiceClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QStringLiteral("Kira Service Manager"), QWebSocketProtocol::VersionLatest, this))
{
    QSettings settings;
    m_endpoint = QUrl(settings.value(EndpointKey, QStringLiteral("ws://127.0.0.1:%1").arg(ServiceProtocol::DefaultPort)).toString());
    m_token = settings.value(TokenKey).toString();
    connect(m_socket, &QWebSocket::connected, this, &ManagerServiceClient::authenticate);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &ManagerServiceClient::handleTextMessage);
    connect(m_socket, &QWebSocket::disconnected, this, [this] {
        m_authenticated = false;
        m_modelListAvailable = false;
        m_generationModels.clear(); m_upscaleModels.clear(); m_llmChatModels.clear();
        failActiveInstalls(tr("Service connection was closed"));
        emit modelsChanged();
        emit serviceStateChanged(QStringLiteral("offline"), tr("Service connection was closed"));
    });
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit serviceStateChanged(QStringLiteral("connection-error"), tr("Could not connect to %1: %2").arg(m_endpoint.toString(), m_socket->errorString()));
    });
}

QUrl ManagerServiceClient::endpoint() const { return m_endpoint; }
QString ManagerServiceClient::token() const { return m_token; }

void ManagerServiceClient::setConfiguration(const QUrl &endpoint, const QString &token, bool persist)
{
    m_endpoint = endpoint; m_token = token;
    if (persist) { QSettings settings; settings.setValue(EndpointKey, endpoint.toString()); settings.setValue(TokenKey, token); }
}

void ManagerServiceClient::connectToService()
{
    m_authenticated = false;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    emit serviceStateChanged(QStringLiteral("connecting"), tr("Connecting to service…"));
    m_socket->open(m_endpoint);
}

bool ManagerServiceClient::isAuthenticated() const { return m_authenticated; }
bool ManagerServiceClient::modelListAvailable() const { return m_modelListAvailable; }
QList<ManagedModelInfo> ManagerServiceClient::models(const QString &category) const
{
    if (category == QStringLiteral("image-generation")) return m_generationModels;
    if (category == QStringLiteral("upscaler")) return m_upscaleModels;
    if (category == QStringLiteral("llm-chat")) return m_llmChatModels;
    return {};
}
QList<ManagedInstall> ManagerServiceClient::installs() const { return m_installs; }
void ManagerServiceClient::requestModels() { if (m_authenticated) m_socket->sendTextMessage(QStringLiteral("{\"type\":\"models.list\"}")); }

bool ManagerServiceClient::installModel(const QString &category, const QUrl &url, const QString &sha256)
{
    if (!m_authenticated) return false;
    const QString installId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_installs.append({installId, category, url, ManagedInstallState::Requesting, 0, {}, {}});
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("models.install")}, {QStringLiteral("installId"), installId},
                        {QStringLiteral("category"), category}, {QStringLiteral("url"), url.toString()}};
    if (!sha256.isEmpty()) message.insert(QStringLiteral("sha256"), sha256);
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    emit installsChanged();
    return true;
}

void ManagerServiceClient::authenticate()
{
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument({{QStringLiteral("type"), QStringLiteral("authenticate")},
        {QStringLiteral("version"), ServiceProtocol::Version}, {QStringLiteral("token"), m_token}}).toJson(QJsonDocument::Compact)));
}

void ManagerServiceClient::handleTextMessage(const QString &message)
{
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8()); if (!document.isObject()) return;
    const QJsonObject object = document.object(); const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("authenticated")) { m_authenticated = true; requestModels(); emit serviceStateChanged(QStringLiteral("connected"), tr("Connected to service")); return; }
    if (type == QStringLiteral("models.list.result")) {
        m_generationModels.clear(); m_upscaleModels.clear(); m_llmChatModels.clear();
        for (const QJsonValue &categoryValue : object.value(QStringLiteral("categories")).toArray()) {
            const QJsonObject category = categoryValue.toObject(); QList<ManagedModelInfo> *modelsForCategory = nullptr;
            const QString name = category.value(QStringLiteral("category")).toString();
            if (name == QStringLiteral("image-generation")) modelsForCategory = &m_generationModels;
            else if (name == QStringLiteral("upscaler")) modelsForCategory = &m_upscaleModels;
            else if (name == QStringLiteral("llm-chat")) modelsForCategory = &m_llmChatModels;
            if (!modelsForCategory) continue;
            for (const QJsonValue &modelValue : category.value(QStringLiteral("models")).toArray()) {
                const QJsonObject model = modelValue.toObject();
                modelsForCategory->append({model.value(QStringLiteral("id")).toString(), model.value(QStringLiteral("name")).toString(),
                    model.value(QStringLiteral("fileName")).toString(), model.value(QStringLiteral("bytes")).toVariant().toLongLong()});
            }
        }
        m_modelListAvailable = true; emit modelsChanged(); return;
    }
    if (type == QStringLiteral("models.install.accepted")) { if (auto *install = findInstall(object.value(QStringLiteral("installId")).toString())) { install->state = ManagedInstallState::Accepted; emit installsChanged(); } return; }
    if (type == QStringLiteral("models.install.progress")) { if (auto *install = findInstall(object.value(QStringLiteral("installId")).toString())) {
        const QString state = object.value(QStringLiteral("state")).toString();
        install->state = state == QStringLiteral("queued") ? ManagedInstallState::Queued : state == QStringLiteral("verifying") ? ManagedInstallState::Verifying : ManagedInstallState::Downloading;
        install->bytesDownloaded = object.value(QStringLiteral("bytesDownloaded")).toVariant().toLongLong();
        install->totalBytes = object.contains(QStringLiteral("totalBytes")) ? std::optional<qint64>(object.value(QStringLiteral("totalBytes")).toVariant().toLongLong()) : std::nullopt;
        emit installsChanged(); } return; }
    if (type == QStringLiteral("models.install.completed")) { if (auto *install = findInstall(object.value(QStringLiteral("installId")).toString())) { install->state = ManagedInstallState::Completed; install->message = tr("Installed"); emit installsChanged(); } requestModels(); return; }
    if (type == QStringLiteral("error")) {
        const QString installId = object.value(QStringLiteral("installId")).toString();
        if (auto *install = findInstall(installId)) { install->state = ManagedInstallState::Failed; install->message = object.value(QStringLiteral("message")).toString(); emit installsChanged(); return; }
        if (!m_authenticated) { emit serviceStateChanged(QStringLiteral("authentication-failed"), object.value(QStringLiteral("message")).toString()); m_socket->close(); }
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
    for (ManagedInstall &install : m_installs) if (install.state != ManagedInstallState::Completed && install.state != ManagedInstallState::Failed) { install.state = ManagedInstallState::Failed; install.message = message; changed = true; }
    if (changed) emit installsChanged();
}
