#include "ManageModelsWindow.h"
#include <QLineEdit>
#include "AddModelDialog.h"
#include "ManagerServiceClient.h"
#include "ModelOptionsWidget.h"
#include "ModelRoutingDialog.h"
#include "ServiceConnectionDialog.h"
#include <Ardoise/IconProvider.h>
#include <Ardoise/Panel.h>
#include <Ardoise/PrimaryButton.h>
#include <Ardoise/TreeView.h>
#include <QApplication>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace
{
const QList<QPair<QString, QString>> categories{{"llm-chat", "LLM"},
                                                {"image-generation", "Image generation"},
                                                {"upscaler", "Upscaler"}, {"companion", "Companion"}};
QString sizeText(qint64 bytes) { return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1); }
} // namespace
ManageModelsWindow::ManageModelsWindow(QWidget *parent, ManagerServiceClient *client)
    : Ardoise::MainWindow(parent), m_client(client ? client : new ManagerServiceClient(this))
{
    setWindowTitle(tr("Nukleus Manager"));
    setWindowIcon(QApplication::windowIcon().isNull()
                      ? QIcon(QStringLiteral(":/manager/icons/manager-32.png"))
                      : QApplication::windowIcon());
    setFixedSize(1100, 640);
    QSettings settings;
    m_client->setConnection(settings.value("Service/Host", m_client->host()).toString(),
                            settings.value("Service/Token", m_client->token()).toString());
    restoreGeometry(settings.value("MainWindow/Geometry").toByteArray());
    setWindowState(Qt::WindowNoState);
    auto *body = new QWidget(this);
    setContentWidget(body);
    auto *layout = new QVBoxLayout(body);
    m_status = new QLabel(tr("Connecting to the local Nukleus…"), body);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_splitter = new QSplitter(Qt::Horizontal, body);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter, 1);
    auto *library = new Ardoise::Panel(m_splitter);
    auto *left = new QVBoxLayout(library);
    left->addWidget(new QLabel(tr("Installed models"), library));
    m_tree = new Ardoise::TreeView(library);
    m_tree->setObjectName("installedModelsTree");
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeModel = new QStandardItemModel(m_tree);
    m_tree->setModel(m_treeModel);
    left->addWidget(m_tree, 1);
    auto *actions = new QHBoxLayout;
    m_add = new Ardoise::PrimaryButton(tr("Add model"), library);
    m_add->setObjectName("addModel");
    m_add->setIcon(Ardoise::IconProvider::icon(Ardoise::Icon::Plus));
    m_delete = new QPushButton(tr("Delete"), library);
    m_delete->setObjectName("deleteModel");
    actions->addWidget(m_add);
    actions->addWidget(m_delete);
    left->addLayout(actions);
    m_attach = new QPushButton(tr("Optional files…"), library);
    m_attach->setObjectName("attachModelFiles");
    left->addWidget(m_attach);
    m_presets = new QPushButton(tr("Presets…"), library);
    m_presets->setObjectName("modelPresets");
    left->addWidget(m_presets);
    auto *configuration = new Ardoise::Panel(m_splitter);
    auto* panelLayout = new QVBoxLayout(configuration);
    m_configurationBody = new QWidget(configuration);
    m_configurationBody->setObjectName("modelConfigurationBody");
    panelLayout->addWidget(m_configurationBody, 1);
    auto *right = new QVBoxLayout(m_configurationBody);
    right->setContentsMargins(0, 0, 0, 0);
    m_configurationBody->hide();
    right->addWidget(new QLabel(tr("Model configuration"), configuration));
    auto *scroll = new QScrollArea(configuration);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *contentLayout = new QVBoxLayout(content);
    m_details = new QLabel(tr("Select a model to view its recommended options."), content);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    contentLayout->addWidget(m_details);
    auto* displayNameLabel = new QLabel(tr("Display name:"), content);
    contentLayout->addWidget(displayNameLabel);
    m_displayName = new QLineEdit(content);
    m_displayName->setObjectName("modelDisplayName");
    m_displayName->setMaxLength(256);
    m_displayName->setPlaceholderText(tr("Required display name"));
    displayNameLabel->setBuddy(m_displayName);
    contentLayout->addWidget(m_displayName);
    m_mmprojLabel = new QLabel(tr("MMPROJ companion:"), content);
    m_mmproj = new QComboBox(content);
    m_mmproj->setObjectName("modelMmprojCompanion");
    contentLayout->addWidget(m_mmprojLabel);
    contentLayout->addWidget(m_mmproj);
    connect(m_mmproj, &QComboBox::currentIndexChanged, this, [this] {
        m_mmproj->setProperty("selectedModelId", m_mmproj->currentData());
        m_dirty = m_options->options() != m_originalOptions
            || m_options->explicitContent() != m_originalExplicitContent
            || m_displayName->text().trimmed() != m_originalDisplayName
            || m_mmproj->currentData().toString() != m_originalMmproj;
        updateActions();
    });
    m_options = new ModelOptionsWidget(content);
    m_options->hide();
    contentLayout->addWidget(m_options);
    contentLayout->addStretch();
    scroll->setWidget(content);
    right->addWidget(scroll, 1);
    m_save = new Ardoise::PrimaryButton(tr("Save"), configuration);
    m_save->setObjectName("saveOptions");
    m_save->setIcon(Ardoise::IconProvider::icon(Ardoise::Icon::Save));
    right->addWidget(m_save);
    m_splitter->setSizes({390, 850});
    m_splitter->restoreState(settings.value("MainWindow/ModelsSplitter").toByteArray());
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &ManageModelsWindow::selectionChanged);
    connect(m_options, &ModelOptionsWidget::optionsChanged, this,
            [this]
            {
                m_dirty = m_options->options() != m_originalOptions
                    || m_options->explicitContent() != m_originalExplicitContent
                    || m_displayName->text().trimmed() != m_originalDisplayName
                    || m_mmproj->currentData().toString() != m_originalMmproj;
                updateActions();
            });
    connect(m_add, &QPushButton::clicked, this, &ManageModelsWindow::addModel);
    connect(m_displayName, &QLineEdit::textChanged, this, [this] {
        m_dirty = m_options->options() != m_originalOptions
            || m_options->explicitContent() != m_originalExplicitContent
            || m_displayName->text().trimmed() != m_originalDisplayName
                    || m_mmproj->currentData().toString() != m_originalMmproj;
        updateActions();
    });
    connect(m_attach, &QPushButton::clicked, this, &ManageModelsWindow::attachFiles);
    connect(m_presets, &QPushButton::clicked, this, &ManageModelsWindow::editPresets);
    connect(m_delete, &QPushButton::clicked, this, &ManageModelsWindow::deleteSelectedModel);
    connect(m_save, &QPushButton::clicked, this, &ManageModelsWindow::saveOptions);
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ManageModelsWindow::populateModels);
    connect(m_client, &ManagerServiceClient::modelRoutingChanged, this, &ManageModelsWindow::updateActions);
    connect(m_client, &ManagerServiceClient::modelRoutingFailed, m_status, &QLabel::setText);
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ManageModelsWindow::refreshCompanions);
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this,
            [this](const QString &state, const QString &message)
            {
                m_status->setText(message);
                if (state == "connected") m_connectionPrompted = true;
                if (state == "connection-error" || state == "authentication-failed" || state == "offline")
                    showConnectionDialog();
                if ((!m_client->isAuthenticated() || state == "operation-error") && m_saving)
                {
                    m_saving = false;
                    m_afterSave = {};
                }
                updateActions();
            });
    connect(m_client, &ManagerServiceClient::modelOperationFailed, this,
            [this](const QString &id, const QString &message)
            {
                m_status->setText(message);
                if (id == m_selectedId)
                {
                    m_saving = false;
                    m_afterSave = {};
                    updateActions();
                }
            });
    connect(m_client, &ManagerServiceClient::modelConfigured, this,
            [this](const QString &id, const QJsonObject &options, bool explicitContent)
            {
                if (id != m_selectedId)
                    return;
                m_originalOptions = options;
                m_originalExplicitContent = explicitContent;
                m_originalDisplayName = m_displayName->text().trimmed();
                m_originalMmproj = m_mmproj->currentData().toString();
                m_dirty = false;
                m_saving = false;
                m_status->setText(tr("Recommended options saved."));
                updateActions();
                auto continuation = std::move(m_afterSave);
                m_afterSave = {};
                if (continuation)
                    continuation();
            });
    connect(m_client, &ManagerServiceClient::modelDeleted, this,
            [this] { m_status->setText(tr("Model deleted.")); });
    populateModels();
    updateActions();
    m_client->connectToService();
}
void ManageModelsWindow::showConnectionDialog()
{
    if (m_connectionDialog || m_connectionPrompted) return;
    m_connectionPrompted = true;
    m_client->setAutomaticReconnect(false);
    m_connectionDialog = new ServiceConnectionDialog(m_client, this);
    connect(m_connectionDialog, &QDialog::finished, this, [this](int result) {
        m_connectionDialog->deleteLater();
        m_connectionDialog = nullptr;
        m_client->setAutomaticReconnect(true);
        if (result == QDialog::Rejected) m_client->connectToService();
    });
    m_connectionDialog->open();
}

void ManageModelsWindow::populateModels()
{
    QSignalBlocker blocker(m_tree->selectionModel());
    m_treeModel->clear();
    auto *root = new QStandardItem(tr("Models"));
    root->setEditable(false);
    m_treeModel->appendRow(root);
    for (const auto &category : categories)
    {
        auto *branch = new QStandardItem(tr(qPrintable(category.second)));
        branch->setEditable(false);
        branch->setIcon(Ardoise::IconProvider::icon(Ardoise::Icon::Folder));
        branch->setData(category.first, Qt::UserRole + 2);
        root->appendRow(branch);
        QHash<QString, QStandardItem*> subtypeBranches;
        if (category.first == "companion") {
            for (const auto& subtype : {QString("vae"), QString("mmproj")}) {
                auto* group = new QStandardItem(subtype == "vae" ? tr("VAE") : tr("MMPROJ (vision projector)"));
                group->setEditable(false);
                group->setIcon(Ardoise::IconProvider::icon(Ardoise::Icon::Folder));
                group->setData(category.first, Qt::UserRole + 2);
                group->setData(subtype, Qt::UserRole + 3);
                branch->appendRow(group);
                subtypeBranches.insert(subtype, group);
            }
        }
        for (const auto &model : m_client->models(category.first))
        {
            auto *item = new QStandardItem(model.displayLabel());
            item->setEditable(false);
            item->setIcon(Ardoise::IconProvider::icon(Ardoise::Icon::File));
            item->setData(model.id, Qt::UserRole + 1);
            item->setData(category.first, Qt::UserRole + 2);
            item->setToolTip(model.fileName);
            auto* destination = subtypeBranches.value(model.recommendedOptions.value("subtype").toString(), branch);
            destination->appendRow(item);
            auto files = model.companionFiles;
            if (!model.companionFileName.isEmpty()) files.insert(model.companionRole, model.companionFileName);
            for (auto it = files.begin(); it != files.end(); ++it) {
                if (category.first == "image-generation"
                    && (it.key() == "clip-l" || it.key() == "clip-g" || it.key() == "mmproj")) continue;
                auto *companion = new QStandardItem(it.key() + ": " + QFileInfo(it.value().toString()).fileName());
                companion->setEditable(false);
                companion->setData(model.id, Qt::UserRole + 1);
                companion->setData(category.first, Qt::UserRole + 2);
                item->appendRow(companion);
            }
            if (!model.mmprojModelId.isEmpty())
                for (const auto& target : m_client->models("companion"))
                    if (target.id == model.mmprojModelId) {
                        auto* link = new QStandardItem("MMPROJ: " + target.displayLabel());
                        link->setEditable(false);
                        link->setData(model.id, Qt::UserRole + 1);
                        item->appendRow(link);
                    }
        }
    }
    m_tree->expandAll();
    selectModel(m_selectedId);
    const bool exists =
        !m_selectedId.isEmpty() && m_tree->currentIndex().data(Qt::UserRole + 1).toString() == m_selectedId;
    if (!exists && m_client->modelListAvailable())
        showSelection({});
    else if (!m_dirty && !m_saving)
        showSelection(m_selectedId);
    updateActions();
}
void ManageModelsWindow::selectModel(const QString &id)
{
    const auto found = m_treeModel->match(m_treeModel->index(0, 0), Qt::UserRole + 1, id, 1,
                                          Qt::MatchExactly | Qt::MatchRecursive);
    QSignalBlocker blocker(m_tree->selectionModel());
    m_tree->setCurrentIndex(id.isEmpty() || found.isEmpty() ? QModelIndex() : found.first());
}
void ManageModelsWindow::showSelection(const QString &id)
{
    m_selectedId = id;
    m_dirty = false;
    m_originalOptions = {};
    m_originalDisplayName.clear();
    const QSignalBlocker displayNameBlocker(m_displayName);
    m_displayName->clear();
    m_displayName->setEnabled(false);
    m_configurationBody->hide();
    m_options->hide();
    m_details->setText(tr("Select a model to view its recommended options."));
    for (const auto &category : categories)
        for (const auto &model : m_client->models(category.first))
        {
            if (model.id != id)
                continue;
            m_originalOptions = model.recommendedOptions;
            m_originalExplicitContent = model.explicitContent;
            m_originalDisplayName = model.displayName;
            m_displayName->setText(model.displayName);
            m_displayName->setEnabled(true);
            m_options->setOptions(category.first, model.recommendedOptions, model.explicitContent);
            m_configurationBody->show();
            m_options->setVisible(category.first != "upscaler");
            m_mmprojLabel->setVisible(category.first == "llm-chat");
            m_mmproj->setVisible(category.first == "llm-chat");
            m_originalMmproj = model.mmprojModelId;
            m_mmproj->setProperty("selectedModelId", m_originalMmproj);
            refreshCompanions();
            m_details->setText(
                tr("%1\n%2 · %3")
                    .arg(model.displayLabel(), model.fileName, sizeText(model.fileSize)));
        }
    updateActions();
}
void ManageModelsWindow::selectionChanged()
{
    const QString id = m_tree->currentIndex().data(Qt::UserRole + 1).toString();
    if (id == m_selectedId)
        return;
    const QPersistentModelIndex requested = m_tree->currentIndex();
    const QString old = m_selectedId;
    selectModel(old);
    const auto change = [this, id, requested]
    {
        QSignalBlocker blocker(m_tree->selectionModel());
        m_tree->setCurrentIndex(requested);
        showSelection(id);
    };
    if (guardChanges(change))
        change();
}
bool ManageModelsWindow::guardChanges(std::function<void()> continuation)
{
    if (m_saving)
        return false;
    if (!m_dirty)
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Unsaved recommendations"), tr("Save changes to this model before continuing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Discard)
    {
        showSelection(m_selectedId);
        return true;
    }
    if (answer == QMessageBox::Save && m_client->isAuthenticated() && m_options->isValid())
    {
        m_afterSave = std::move(continuation);
        saveOptions();
    }
    return false;
}
void ManageModelsWindow::updateActions()
{
    m_presets->setEnabled(m_client->isAuthenticated() && m_client->modelRouting().has_value() && !m_saving);
    m_presets->setToolTip(m_client->supportsModelRouting() ? QString() : tr("Upgrade Nukleus to configure presets."));
    m_tree->setEnabled(!m_saving);
    m_options->setEnabled(m_client->isAuthenticated() && !m_saving);
    m_displayName->setEnabled(m_client->isAuthenticated() && !m_saving && !m_selectedId.isEmpty());
    m_mmproj->setEnabled(m_client->isAuthenticated() && !m_saving && m_selectedId.startsWith("llm-chat:"));
    m_add->setEnabled(m_client->isAuthenticated() && !m_saving);
    m_attach->setEnabled(m_client->isAuthenticated() && !m_saving &&
        (m_selectedId.startsWith("image-generation:")));
    m_delete->setEnabled(m_client->modelListAvailable() && m_client->isAuthenticated() && !m_saving &&
                         !m_selectedId.isEmpty());
    m_save->setEnabled(m_client->modelListAvailable() && m_client->isAuthenticated() && !m_saving &&
                       !m_selectedId.isEmpty() && !m_displayName->text().trimmed().isEmpty() && m_options->isValid() &&
                       (m_dirty || (m_originalOptions.isEmpty() && !m_selectedId.startsWith("upscaler:"))));
}
void ManageModelsWindow::saveOptions()
{
    if (!m_options->isValid())
        return;
    if (m_client->configureModel(m_selectedId, m_options->options(), m_selectedId.startsWith("upscaler:") || m_selectedId.startsWith("companion:") ? std::nullopt : std::optional<bool>(m_options->explicitContent()), m_displayName->text(),
        m_selectedId.startsWith("llm-chat:") ? std::optional<QString>(m_mmproj->currentData().toString()) : std::nullopt))
    {
        m_saving = true;
        m_status->setText(tr("Saving recommended options…"));
        updateActions();
    }
    else
        m_afterSave = {};
}
void ManageModelsWindow::addModel()
{
    const auto open = [this]
    {
        AddModelDialog dialog(m_client, m_tree->currentIndex().data(Qt::UserRole + 2).toString(), this);
        const auto subtype = m_tree->currentIndex().data(Qt::UserRole + 3).toString();
        if (!subtype.isEmpty()) {
            auto* choice = dialog.findChild<QComboBox*>("subtype");
            if (choice) choice->setCurrentIndex(choice->findData(subtype));
        }
        connect(&dialog, &AddModelDialog::modelInstalled, this,
                [this](const QString &id)
                {
                    m_dirty = false;
                    m_selectedId = id;
                });
        dialog.exec();
    };
    if (guardChanges(open))
        open();
}
void ManageModelsWindow::attachFiles()
{
    const auto open = [this] {
        AddModelDialog dialog(m_client, m_selectedId.section(':', 0, 0), this, m_selectedId);
        connect(&dialog, &AddModelDialog::modelInstalled, this, [this](const QString &id) {
            m_client->requestModels();
            selectModel(id);
        });
        dialog.exec();
    };
    if (guardChanges(open)) open();
}

void ManageModelsWindow::editPresets()
{
    const auto open = [this] {
        ModelRoutingDialog dialog(m_client, this);
        dialog.exec();
    };
    if (guardChanges(open)) open();
}

void ManageModelsWindow::deleteSelectedModel()
{
    const auto remove = [this]
    {
        if (QMessageBox::question(this, tr("Delete model"),
                                  tr("Delete this model and its companion files? This cannot be undone."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            m_client->deleteModel(m_selectedId);
    };
    if (guardChanges(remove))
        remove();
}
void ManageModelsWindow::closeEvent(QCloseEvent *event)
{
    if (!m_allowClose && !guardChanges(
                             [this]
                             {
                                 m_allowClose = true;
                                 close();
                             }))
    {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue("MainWindow/Geometry", saveGeometry());
    settings.setValue("MainWindow/ModelsSplitter", m_splitter->saveState());
    Ardoise::MainWindow::closeEvent(event);
}

void ManageModelsWindow::refreshCompanions()
{
    m_options->setModels(m_client->models("companion") + m_client->models("llm-chat"));
    const QSignalBlocker blocker(m_mmproj);
    const auto selected = m_mmproj->property("selectedModelId").toString();
    m_mmproj->clear();
    m_mmproj->addItem(tr("None"), QString());
    for (const auto& model : m_client->models("companion"))
        if (model.recommendedOptions.value("subtype") == "mmproj")
            m_mmproj->addItem(model.displayLabel(), model.id);
    int index = m_mmproj->findData(selected);
    if (index < 0 && !selected.isEmpty()) {
        m_mmproj->addItem(tr("Unavailable: %1").arg(selected), selected);
        index = m_mmproj->count() - 1;
    }
    m_mmproj->setCurrentIndex(qMax(0, index));
}
