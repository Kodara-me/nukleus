#include "ManageModelsWindow.h"

#include "ManagerServiceClient.h"
#include "ServiceProtocol.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace
{
QString sizeText(qint64 bytes)
{
    if (bytes < 1024) return QObject::tr("%1 bytes").arg(bytes);
    if (bytes < 1024 * 1024) return QObject::tr("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QString installStateText(const ManagedInstall &install)
{
    switch (install.state) {
    case ManagedInstallState::Requesting: return QObject::tr("Requesting");
    case ManagedInstallState::Accepted: return QObject::tr("Accepted");
    case ManagedInstallState::Queued: return QObject::tr("Queued");
    case ManagedInstallState::Downloading: return QObject::tr("Downloading");
    case ManagedInstallState::Verifying: return QObject::tr("Verifying");
    case ManagedInstallState::Completed: return QObject::tr("Installed");
    case ManagedInstallState::Failed: return install.message.isEmpty() ? QObject::tr("Failed") : install.message;
    }
    return QObject::tr("Failed");
}

bool directHuggingFaceUrl(const QUrl &url)
{
    const QStringList path = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return url.isValid() && url.scheme() == QStringLiteral("https") && url.host().compare(QStringLiteral("huggingface.co"), Qt::CaseInsensitive) == 0
           && path.size() >= 5 && path.at(2) == QStringLiteral("resolve");
}
}

ManageModelsWindow::ManageModelsWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_client(new ManagerServiceClient(this))
{
    setWindowTitle(tr("Kira Service Manager")); resize(760, 700);
    auto *central = new QWidget(this); setCentralWidget(central); auto *layout = new QVBoxLayout(central);
    auto *connection = new QGroupBox(tr("Local Kira Service"), central); auto *connectionLayout = new QFormLayout(connection);
    m_host = new QLineEdit(m_client->endpoint().host().isEmpty() ? QStringLiteral("127.0.0.1") : m_client->endpoint().host(), connection);
    m_host->setPlaceholderText(QStringLiteral("127.0.0.1")); connectionLayout->addRow(tr("Service IP"), m_host);
    m_token = new QLineEdit(m_client->token(), connection); m_token->setEchoMode(QLineEdit::Password); connectionLayout->addRow(tr("Shared token"), m_token);
    auto *connectButton = new QPushButton(tr("Connect"), connection); connectionLayout->addRow(connectButton);
    m_status = new QLabel(connection); m_status->setWordWrap(true); connectionLayout->addRow(m_status); layout->addWidget(connection);
    auto *description = new QLabel(tr("Install direct Hugging Face resolve file URLs. Files remain on this service host."), central); description->setWordWrap(true); layout->addWidget(description);
    auto *modelsGroup = new QGroupBox(tr("Installed models"), central); auto *modelsLayout = new QFormLayout(modelsGroup);
    m_generationModels = new QListWidget(modelsGroup); m_upscaleModels = new QListWidget(modelsGroup); m_llmChatModels = new QListWidget(modelsGroup);
    for (QListWidget *list : {m_generationModels, m_upscaleModels, m_llmChatModels}) list->setMaximumHeight(90);
    modelsLayout->addRow(tr("Image-generation"), m_generationModels); modelsLayout->addRow(tr("Upscaler"), m_upscaleModels); modelsLayout->addRow(tr("LLM-chat"), m_llmChatModels); layout->addWidget(modelsGroup);
    auto *installGroup = new QGroupBox(tr("Install model"), central); auto *installLayout = new QFormLayout(installGroup);
    m_category = new QComboBox(installGroup); m_category->addItem(tr("Image-generation"), QStringLiteral("image-generation")); m_category->addItem(tr("Upscaler"), QStringLiteral("upscaler")); m_category->addItem(tr("LLM-chat"), QStringLiteral("llm-chat"));
    m_url = new QLineEdit(installGroup); m_url->setPlaceholderText(QStringLiteral("https://huggingface.co/owner/repo/resolve/main/model.safetensors"));
    m_sha256 = new QLineEdit(installGroup); m_sha256->setPlaceholderText(tr("Optional 64-character SHA-256"));
    m_installButton = new QPushButton(tr("Install"), installGroup);
    installLayout->addRow(tr("Category"), m_category); installLayout->addRow(tr("File URL"), m_url); installLayout->addRow(tr("SHA-256"), m_sha256); installLayout->addRow(m_installButton); layout->addWidget(installGroup);
    m_installs = new QTreeWidget(central); m_installs->setHeaderLabels({tr("Model"), tr("Status"), tr("Progress")}); m_installs->header()->setSectionResizeMode(QHeaderView::ResizeToContents); m_installs->header()->setStretchLastSection(true); layout->addWidget(m_installs, 1);
    connect(connectButton, &QPushButton::clicked, this, &ManageModelsWindow::connectService); connect(m_installButton, &QPushButton::clicked, this, &ManageModelsWindow::startInstall);
    connect(m_category, &QComboBox::currentIndexChanged, this, &ManageModelsWindow::updateInstallButton); connect(m_url, &QLineEdit::textChanged, this, &ManageModelsWindow::updateInstallButton); connect(m_sha256, &QLineEdit::textChanged, this, &ManageModelsWindow::updateInstallButton);
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ManageModelsWindow::populateModels); connect(m_client, &ManagerServiceClient::installsChanged, this, [this] { populateInstalls(); updateInstallButton(); });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this, [this](const QString &, const QString &message) { m_status->setText(message); updateInstallButton(); });
    populateModels(); populateInstalls(); updateInstallButton(); connectService();
}

bool ManageModelsWindow::validLocalHost(const QString &host) const
{
    if (host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")) return true;
    QHostAddress address; if (!address.setAddress(host) || address.protocol() != QAbstractSocket::IPv4Protocol) return false;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces())
        for (const QNetworkAddressEntry &entry : interface.addressEntries()) if (entry.ip() == address) return true;
    return false;
}

void ManageModelsWindow::connectService()
{
    const QString host = m_host->text().trimmed();
    if (!validLocalHost(host)) { m_status->setText(tr("Enter localhost or an IPv4 address assigned to this computer.")); return; }
    QUrl endpoint; endpoint.setScheme(QStringLiteral("ws")); endpoint.setHost(host); endpoint.setPort(ServiceProtocol::DefaultPort);
    m_client->setConfiguration(endpoint, m_token->text()); m_client->connectToService();
}

void ManageModelsWindow::populateModels()
{
    const auto populate = [this](QListWidget *widget, const QString &category) { widget->clear(); const QList<ManagedModelInfo> models = m_client->models(category); if (models.isEmpty()) { widget->addItem(tr("No installed models")); return; } for (const ManagedModelInfo &model : models) widget->addItem(tr("%1 (%2) — %3").arg(model.name, model.fileName, sizeText(model.fileSize))); };
    populate(m_generationModels, QStringLiteral("image-generation")); populate(m_upscaleModels, QStringLiteral("upscaler")); populate(m_llmChatModels, QStringLiteral("llm-chat"));
}

void ManageModelsWindow::populateInstalls()
{
    m_installs->clear(); for (const ManagedInstall &install : m_client->installs()) { auto *item = new QTreeWidgetItem(m_installs); item->setText(0, install.url.fileName()); item->setText(1, installStateText(install)); if (install.state == ManagedInstallState::Downloading) item->setText(2, install.totalBytes.has_value() ? tr("%1 / %2").arg(sizeText(install.bytesDownloaded), sizeText(*install.totalBytes)) : sizeText(install.bytesDownloaded)); }
}

void ManageModelsWindow::updateInstallButton()
{
    const QUrl url(m_url->text().trimmed()); const QString checksum = m_sha256->text().trimmed();
    const bool checksumValid = checksum.isEmpty() || QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(checksum).hasMatch();
    const QString category = m_category->currentData().toString();
    const QList<ManagedInstall> installs = m_client->installs();
    const bool duplicate = std::any_of(installs.cbegin(), installs.cend(), [&category, &url](const ManagedInstall &install) { return install.category == category && install.url == url && install.state != ManagedInstallState::Completed && install.state != ManagedInstallState::Failed; });
    m_installButton->setEnabled(m_client->isAuthenticated() && directHuggingFaceUrl(url) && checksumValid && !duplicate);
}

void ManageModelsWindow::startInstall()
{
    if (m_client->installModel(m_category->currentData().toString(), QUrl(m_url->text().trimmed()), m_sha256->text().trimmed())) { m_url->clear(); m_sha256->clear(); }
}
