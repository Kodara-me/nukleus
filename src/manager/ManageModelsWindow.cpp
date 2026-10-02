#include "ManageModelsWindow.h"

#include "ManagerServiceClient.h"
#include <kira/service/ServiceProtocol.h>

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
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
    auto *modelsGroup = new QGroupBox(tr("Installed models"), central);
    auto *modelsLayout = new QVBoxLayout(modelsGroup);
    m_models = new QTreeWidget(modelsGroup);
    m_models->setObjectName(QStringLiteral("installedModelsTree"));
    m_models->setHeaderLabels({tr("Models"), tr("File"), tr("Size")});
    m_models->setSelectionMode(QAbstractItemView::SingleSelection);
    m_models->setUniformRowHeights(true);
    m_models->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_models->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_models->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_models->header()->setStretchLastSection(false);
    modelsLayout->addWidget(m_models);
    layout->addWidget(modelsGroup, 1);
    m_deleteButton = new QPushButton(tr("Delete selected model"), modelsGroup);
    m_deleteButton->setEnabled(false);
    modelsLayout->addWidget(m_deleteButton);
    auto *installGroup = new QGroupBox(tr("Install model"), central); auto *installLayout = new QFormLayout(installGroup);
    m_category = new QComboBox(installGroup); m_category->addItem(tr("Image-generation"), QStringLiteral("image-generation")); m_category->addItem(tr("Edition"), QStringLiteral("edition")); m_category->addItem(tr("Upscaler"), QStringLiteral("upscaler")); m_category->addItem(tr("LLM-chat"), QStringLiteral("llm-chat")); m_category->addItem(tr("IP-Adapter"), QStringLiteral("ip-adapter"));
    m_url = new QLineEdit(installGroup); m_url->setPlaceholderText(QStringLiteral("https://example.com/models/model.gguf"));
    m_companionLabel = new QLabel(tr("Companion LoRA URL"), installGroup);
    m_companionUrl = new QLineEdit(installGroup); m_companionUrl->setObjectName(QStringLiteral("companionUrl")); m_companionUrl->setPlaceholderText(tr("Optional companion file URL"));
    m_installButton = new QPushButton(tr("Install"), installGroup);
    installLayout->addRow(tr("Category"), m_category); installLayout->addRow(tr("File URL"), m_url); installLayout->addRow(m_companionLabel, m_companionUrl); installLayout->addRow(m_installButton); layout->addWidget(installGroup);
    m_installs = new QTreeWidget(central); m_installs->setHeaderLabels({tr("Model"), tr("Status"), tr("Progress")}); m_installs->header()->setSectionResizeMode(QHeaderView::ResizeToContents); m_installs->header()->setStretchLastSection(true); layout->addWidget(m_installs, 1);
    connect(m_installButton, &QPushButton::clicked, this, &ManageModelsWindow::startInstall); connect(m_deleteButton, &QPushButton::clicked, this, &ManageModelsWindow::deleteSelectedModel);
    connect(m_category, &QComboBox::currentIndexChanged, this, [this] {
        const QString category = m_category->currentData().toString();
        const bool supportsCompanion = category != QStringLiteral("upscaler");
        m_companionLabel->setText(category == QStringLiteral("llm-chat") ? tr("Optional mmproj URL")
            : category == QStringLiteral("ip-adapter") ? tr("Optional companion LoRA URL") : tr("Optional VAE URL"));
        m_companionUrl->clear();
        m_companionLabel->setVisible(supportsCompanion);
        m_companionUrl->setVisible(supportsCompanion);
        m_companionUrl->setEnabled(supportsCompanion);
        updateInstallButton();
    }); connect(m_url, &QLineEdit::textChanged, this, &ManageModelsWindow::updateInstallButton); connect(m_companionUrl, &QLineEdit::textChanged, this, &ManageModelsWindow::updateInstallButton);
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ManageModelsWindow::populateModels); connect(m_client, &ManagerServiceClient::installsChanged, this, [this] { populateInstalls(); updateInstallButton(); });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this, [this](const QString &, const QString &message) { m_status->setText(message); updateInstallButton(); updateDeleteButton(); });
    connect(m_client, &ManagerServiceClient::modelDeleted, this, [this](const QString &) { m_status->setText(tr("Model deleted.")); });
    connect(m_client, &ManagerServiceClient::modelOperationFailed, this, [this](const QString &, const QString &message) { m_status->setText(message); updateDeleteButton(); });
    connect(m_models, &QTreeWidget::itemSelectionChanged, this, &ManageModelsWindow::updateDeleteButton);
    m_companionLabel->setText(tr("Optional VAE URL")); populateModels(); populateInstalls(); updateInstallButton(); updateDeleteButton(); m_client->connectToService();
}

void ManageModelsWindow::populateModels()
{
    m_models->clear();
    auto *root = new QTreeWidgetItem(m_models, {tr("Models")});
    root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
    const QList<QPair<QString, QString>> categories = {
        {QStringLiteral("edition"), tr("Edition")},
        {QStringLiteral("llm-chat"), tr("LLM")},
        {QStringLiteral("image-generation"), tr("Image generation")},
        {QStringLiteral("upscaler"), tr("Upscaler")},
        {QStringLiteral("ip-adapter"), tr("IP-Adapter")}
    };
    for (const auto &category : categories) {
        auto *categoryItem = new QTreeWidgetItem(root, {category.second});
        categoryItem->setFlags(categoryItem->flags() & ~Qt::ItemIsSelectable);
        for (const ManagedModelInfo &model : m_client->models(category.first)) {
            auto *modelItem = new QTreeWidgetItem(categoryItem, {model.name, model.fileName, sizeText(model.fileSize)});
            modelItem->setData(0, Qt::UserRole, model.id);
            modelItem->setToolTip(1, model.fileName);
            if (!model.companionFileName.isEmpty()) {
                const QString role = model.companionRole.isEmpty() ? tr("Companion") : model.companionRole;
                auto *companion = new QTreeWidgetItem(modelItem, {role, QFileInfo(model.companionFileName).fileName()});
                companion->setToolTip(1, model.companionFileName);
            }
        }
    }
    m_models->expandAll();
    updateDeleteButton();
}

void ManageModelsWindow::populateInstalls()
{
    m_installs->clear(); for (const ManagedInstall &install : m_client->installs()) { auto *item = new QTreeWidgetItem(m_installs); const QString companionFile = install.companionFileName.isEmpty() ? install.companionUrl.fileName() : install.companionFileName; const QString companion = companionFile.isEmpty() ? QString() : tr(" — companion: %1").arg(companionFile); item->setText(0, install.url.fileName() + companion); item->setText(1, installStateText(install)); if (install.state == ManagedInstallState::Downloading) item->setText(2, install.totalBytes.has_value() ? tr("%1 / %2").arg(sizeText(install.bytesDownloaded), sizeText(*install.totalBytes)) : sizeText(install.bytesDownloaded)); }
}

void ManageModelsWindow::updateInstallButton()
{
    const QUrl url(m_url->text().trimmed());
    const QUrl companionUrl(m_companionUrl->text().trimmed());
    const QString category = m_category->currentData().toString();
    const bool companionValid = category == QStringLiteral("upscaler") || m_companionUrl->text().trimmed().isEmpty() || validModelUrl(companionUrl);
    const QList<ManagedInstall> installs = m_client->installs();
    const bool duplicate = std::any_of(installs.cbegin(), installs.cend(), [&category, &url](const ManagedInstall &install) { return install.category == category && install.url == url && install.state != ManagedInstallState::Completed && install.state != ManagedInstallState::Failed; });
    m_installButton->setEnabled(m_client->isAuthenticated() && validModelUrl(url) && companionValid && !duplicate);
}

void ManageModelsWindow::startInstall()
{
    const QString category = m_category->currentData().toString();
    if (!m_client->installModel(category, QUrl(m_url->text().trimmed()), category != QStringLiteral("upscaler") ? QUrl(m_companionUrl->text().trimmed()) : QUrl()).isEmpty()) { m_url->clear(); m_companionUrl->clear(); }
}

void ManageModelsWindow::updateDeleteButton()
{
    const auto selected = m_models->selectedItems();
    const bool hasSelection = !selected.isEmpty() && !selected.first()->data(0, Qt::UserRole).toString().isEmpty();
    m_deleteButton->setEnabled(m_client->isAuthenticated() && hasSelection);
}

void ManageModelsWindow::deleteSelectedModel()
{
    const auto selected = m_models->selectedItems();
    const QString id = selected.isEmpty() ? QString() : selected.first()->data(0, Qt::UserRole).toString();
    if (id.isEmpty()) return;
    if (QMessageBox::question(this, tr("Delete model"), tr("Delete %1? This cannot be undone.").arg(id), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
        m_deleteButton->setEnabled(false);
        m_client->deleteModel(id);
    }
}
