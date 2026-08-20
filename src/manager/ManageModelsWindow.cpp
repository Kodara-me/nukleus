#include "ManageModelsWindow.h"

#include "ManagerServiceClient.h"
#include <kira/service/ServiceProtocol.h>

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
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

bool validModelUrl(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty()
           && url.userInfo().isEmpty() && !url.hasFragment() && !url.fileName().isEmpty();
}
}

ManageModelsWindow::ManageModelsWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_client(new ManagerServiceClient(this))
{
    setWindowTitle(tr("Kira Service Manager")); resize(760, 700);
    auto *central = new QWidget(this); setCentralWidget(central); auto *layout = new QVBoxLayout(central);
    auto *connection = new QGroupBox(tr("Local Kira Service"), central); auto *connectionLayout = new QVBoxLayout(connection);
    m_status = new QLabel(tr("Connecting to the local Kira Service…"), connection); m_status->setWordWrap(true); connectionLayout->addWidget(m_status); layout->addWidget(connection);
    auto *description = new QLabel(tr("Install HTTPS model file URLs from Hugging Face or another host. Files remain on this service host."), central); description->setWordWrap(true); layout->addWidget(description);
    auto *modelsGroup = new QGroupBox(tr("Installed models"), central); auto *modelsLayout = new QFormLayout(modelsGroup);
    m_generationModels = new QListWidget(modelsGroup); m_upscaleModels = new QListWidget(modelsGroup); m_llmChatModels = new QListWidget(modelsGroup);
    for (QListWidget *list : {m_generationModels, m_upscaleModels, m_llmChatModels}) { list->setMaximumHeight(90); list->setSelectionMode(QAbstractItemView::SingleSelection); }
    modelsLayout->addRow(tr("Image-generation"), m_generationModels); modelsLayout->addRow(tr("Upscaler"), m_upscaleModels); modelsLayout->addRow(tr("LLM-chat"), m_llmChatModels); layout->addWidget(modelsGroup);
    m_deleteButton = new QPushButton(tr("Delete selected model"), modelsGroup); m_deleteButton->setEnabled(false); modelsLayout->addRow(m_deleteButton);
    auto *installGroup = new QGroupBox(tr("Install model"), central); auto *installLayout = new QFormLayout(installGroup);
    m_category = new QComboBox(installGroup); m_category->addItem(tr("Image-generation"), QStringLiteral("image-generation")); m_category->addItem(tr("Upscaler"), QStringLiteral("upscaler")); m_category->addItem(tr("LLM-chat"), QStringLiteral("llm-chat"));
    m_url = new QLineEdit(installGroup); m_url->setPlaceholderText(QStringLiteral("https://example.com/models/model.gguf"));
    m_sha256 = new QLineEdit(installGroup); m_sha256->setPlaceholderText(tr("Optional 64-character SHA-256"));
    m_installButton = new QPushButton(tr("Install"), installGroup);
    installLayout->addRow(tr("Category"), m_category); installLayout->addRow(tr("File URL"), m_url); installLayout->addRow(tr("SHA-256"), m_sha256); installLayout->addRow(m_installButton); layout->addWidget(installGroup);
    m_installs = new QTreeWidget(central); m_installs->setHeaderLabels({tr("Model"), tr("Status"), tr("Progress")}); m_installs->header()->setSectionResizeMode(QHeaderView::ResizeToContents); m_installs->header()->setStretchLastSection(true); layout->addWidget(m_installs, 1);
    connect(m_installButton, &QPushButton::clicked, this, &ManageModelsWindow::startInstall); connect(m_deleteButton, &QPushButton::clicked, this, &ManageModelsWindow::deleteSelectedModel);
    connect(m_category, &QComboBox::currentIndexChanged, this, &ManageModelsWindow::updateInstallButton); connect(m_url, &QLineEdit::textChanged, this, &ManageModelsWindow::updateInstallButton); connect(m_sha256, &QLineEdit::textChanged, this, &ManageModelsWindow::updateInstallButton);
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ManageModelsWindow::populateModels); connect(m_client, &ManagerServiceClient::installsChanged, this, [this] { populateInstalls(); updateInstallButton(); });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this, [this](const QString &, const QString &message) { m_status->setText(message); updateInstallButton(); updateDeleteButton(); });
    connect(m_client, &ManagerServiceClient::modelDeleted, this, [this](const QString &) { m_status->setText(tr("Model deleted.")); });
    connect(m_client, &ManagerServiceClient::modelOperationFailed, this, [this](const QString &, const QString &message) { m_status->setText(message); updateDeleteButton(); });
    for (QListWidget *list : {m_generationModels, m_upscaleModels, m_llmChatModels}) {
        connect(list, &QListWidget::itemSelectionChanged, this, [this, list] {
            if (list->currentItem() && list->currentItem()->data(Qt::UserRole).isValid()) {
                for (QListWidget *other : {m_generationModels, m_upscaleModels, m_llmChatModels}) if (other != list) other->clearSelection();
            }
            updateDeleteButton();
        });
    }
    populateModels(); populateInstalls(); updateInstallButton(); updateDeleteButton(); m_client->connectToService();
}

void ManageModelsWindow::populateModels()
{
    const auto populate = [this](QListWidget *widget, const QString &category) { widget->clear(); const QList<ManagedModelInfo> models = m_client->models(category); if (models.isEmpty()) { auto *item = new QListWidgetItem(tr("No installed models"), widget); item->setFlags(Qt::NoItemFlags); return; } for (const ManagedModelInfo &model : models) { auto *item = new QListWidgetItem(tr("%1 (%2) — %3").arg(model.name, model.fileName, sizeText(model.fileSize)), widget); item->setData(Qt::UserRole, model.id); } };
    populate(m_generationModels, QStringLiteral("image-generation")); populate(m_upscaleModels, QStringLiteral("upscaler")); populate(m_llmChatModels, QStringLiteral("llm-chat"));
    updateDeleteButton();
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
    m_installButton->setEnabled(m_client->isAuthenticated() && validModelUrl(url) && checksumValid && !duplicate);
}

void ManageModelsWindow::startInstall()
{
    if (!m_client->installModel(m_category->currentData().toString(), QUrl(m_url->text().trimmed()), m_sha256->text().trimmed()).isEmpty()) { m_url->clear(); m_sha256->clear(); }
}

void ManageModelsWindow::updateDeleteButton()
{
    bool hasSelection = false;
    for (QListWidget *list : {m_generationModels, m_upscaleModels, m_llmChatModels}) {
        if (list->currentItem() && list->currentItem()->data(Qt::UserRole).isValid()) { hasSelection = true; break; }
    }
    m_deleteButton->setEnabled(m_client->isAuthenticated() && hasSelection);
}

void ManageModelsWindow::deleteSelectedModel()
{
    QString id;
    for (QListWidget *list : {m_generationModels, m_upscaleModels, m_llmChatModels}) {
        if (list->currentItem()) {
            id = list->currentItem()->data(Qt::UserRole).toString();
            if (!id.isEmpty()) break;
        }
    }
    if (id.isEmpty()) return;
    if (QMessageBox::question(this, tr("Delete model"), tr("Delete %1? This cannot be undone.").arg(id), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
        m_deleteButton->setEnabled(false);
        m_client->deleteModel(id);
    }
}
