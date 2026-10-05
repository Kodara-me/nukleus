#include "ManageModelsWindow.h"
#include <QLineEdit>
#include "AddModelDialog.h"
#include "ManagerServiceClient.h"
#include "ModelOptionsWidget.h"
#include "ModelStylesDialog.h"
#include "ServiceConnectionDialog.h"
#include <KiraUi/IconProvider.h>
#include <KiraUi/Panel.h>
#include <KiraUi/PrimaryButton.h>
#include <KiraUi/TreeView.h>
#include <QApplication>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
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
const QList<QPair<QString, QString>> categories{{"edition", "Edition"},
                                                {"llm-chat", "LLM"},
                                                {"image-generation", "Image generation"},
                                                {"upscaler", "Upscaler"},
                                                {"ip_adapter", "IP-Adapter"}, {"clip_vision", "CLIP Vision"},
                                                {"photomaker", "PhotoMaker"},
                                                {"prompt-enhancer", "Prompt Enhancer"}};
QString sizeText(qint64 bytes) { return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1); }
} // namespace
ManageModelsWindow::ManageModelsWindow(QWidget *parent, ManagerServiceClient *client)
    : KiraUi::MainWindow(parent), m_client(client ? client : new ManagerServiceClient(this))
{
    setWindowTitle(tr("Kira Service Manager"));
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
    m_status = new QLabel(tr("Connecting to the local Kira Service…"), body);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_splitter = new QSplitter(Qt::Horizontal, body);
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter, 1);
    auto *library = new KiraUi::Panel(m_splitter);
    auto *left = new QVBoxLayout(library);
    left->addWidget(new QLabel(tr("Installed models"), library));
    m_tree = new KiraUi::TreeView(library);
    m_tree->setObjectName("installedModelsTree");
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeModel = new QStandardItemModel(m_tree);
    m_tree->setModel(m_treeModel);
    left->addWidget(m_tree, 1);
    auto *actions = new QHBoxLayout;
    m_add = new KiraUi::PrimaryButton(tr("Add model"), library);
    m_add->setObjectName("addModel");
    m_add->setIcon(KiraUi::IconProvider::icon(KiraUi::Icon::Plus));
    m_delete = new QPushButton(tr("Delete"), library);
    m_delete->setObjectName("deleteModel");
    actions->addWidget(m_add);
    actions->addWidget(m_delete);
    left->addLayout(actions);
    m_attach = new QPushButton(tr("Optional files…"), library);
    m_attach->setObjectName("attachModelFiles");
    left->addWidget(m_attach);
    m_styles = new QPushButton(tr("Styles…"), library);
    m_styles->setObjectName("modelStyles");
    left->addWidget(m_styles);
    auto *configuration = new KiraUi::Panel(m_splitter);
    auto *right = new QVBoxLayout(configuration);
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
    auto* displayNameLabel = new QLabel(tr("Display name (optional):"), content);
    contentLayout->addWidget(displayNameLabel);
    m_displayName = new QLineEdit(content);
    m_displayName->setObjectName("modelDisplayName");
    m_displayName->setMaxLength(256);
    m_displayName->setPlaceholderText(tr("Filename when empty"));
    displayNameLabel->setBuddy(m_displayName);
    contentLayout->addWidget(m_displayName);
    m_options = new ModelOptionsWidget(content);
    m_options->hide();
    contentLayout->addWidget(m_options);
    auto *hint =
        new QLabel(tr("Recommended values for this model. Applications can override them."), content);
    hint->setWordWrap(true);
    contentLayout->addWidget(hint);
    contentLayout->addStretch();
    scroll->setWidget(content);
    right->addWidget(scroll, 1);
    m_save = new KiraUi::PrimaryButton(tr("Save recommended options"), configuration);
    m_save->setObjectName("saveOptions");
    m_save->setIcon(KiraUi::IconProvider::icon(KiraUi::Icon::Save));
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
                    || m_displayName->text().trimmed() != m_originalDisplayName;
                updateActions();
            });
    connect(m_add, &QPushButton::clicked, this, &ManageModelsWindow::addModel);
    connect(m_displayName, &QLineEdit::textChanged, this, [this] {
        m_dirty = m_options->options() != m_originalOptions
            || m_options->explicitContent() != m_originalExplicitContent
            || m_displayName->text().trimmed() != m_originalDisplayName;
        updateActions();
    });
    connect(m_attach, &QPushButton::clicked, this, &ManageModelsWindow::attachFiles);
    connect(m_styles, &QPushButton::clicked, this, &ManageModelsWindow::editStyles);
    connect(m_delete, &QPushButton::clicked, this, &ManageModelsWindow::deleteSelectedModel);
    connect(m_save, &QPushButton::clicked, this, &ManageModelsWindow::saveOptions);
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ManageModelsWindow::populateModels);
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
        branch->setIcon(KiraUi::IconProvider::icon(KiraUi::Icon::Folder));
        branch->setData(category.first, Qt::UserRole + 2);
        root->appendRow(branch);
        for (const auto &model : m_client->models(category.first))
        {
            auto *item = new QStandardItem(model.displayLabel());
            item->setEditable(false);
            item->setIcon(KiraUi::IconProvider::icon(KiraUi::Icon::File));
            item->setData(model.id, Qt::UserRole + 1);
            item->setData(category.first, Qt::UserRole + 2);
            item->setToolTip(model.fileName);
            branch->appendRow(item);
            auto files = model.companionFiles;
            if (!model.companionFileName.isEmpty()) files.insert(model.companionRole, model.companionFileName);
            for (auto it = files.begin(); it != files.end(); ++it) {
                auto *companion = new QStandardItem(it.key() + ": " + QFileInfo(it.value().toString()).fileName());
                companion->setEditable(false);
                companion->setData(model.id, Qt::UserRole + 1);
                companion->setData(category.first, Qt::UserRole + 2);
                item->appendRow(companion);
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
            m_options->show();
            m_details->setText(
                tr("%1\n%2 · %3\n%4")
                    .arg(model.displayLabel(), model.fileName, sizeText(model.fileSize),
                         model.recommendedOptions.isEmpty()
                             ? tr("No saved recommendations. Save to attach the proposed values.")
                             : tr("Recommended options")));
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
    bool generationSelected = false;
    for (const auto &model : m_client->models("image-generation"))
        if (model.id == m_selectedId) generationSelected = true;
    m_styles->setEnabled(m_client->isAuthenticated() && m_client->modelListAvailable() && !m_saving && generationSelected);
    m_tree->setEnabled(!m_saving);
    m_options->setEnabled(m_client->isAuthenticated() && !m_saving);
    m_displayName->setEnabled(m_client->isAuthenticated() && !m_saving && !m_selectedId.isEmpty());
    m_add->setEnabled(m_client->isAuthenticated() && !m_saving);
    m_attach->setEnabled(m_client->isAuthenticated() && !m_saving &&
        (m_selectedId.startsWith("image-generation:") || m_selectedId.startsWith("edition:") || m_selectedId.startsWith("ip-adapter:")
         || m_selectedId.startsWith("ip_adapter:")));
    m_delete->setEnabled(m_client->modelListAvailable() && m_client->isAuthenticated() && !m_saving &&
                         !m_selectedId.isEmpty());
    m_save->setEnabled(m_client->modelListAvailable() && m_client->isAuthenticated() && !m_saving &&
                       !m_selectedId.isEmpty() && m_options->isValid() &&
                       (m_dirty || m_originalOptions.isEmpty()));
}
void ManageModelsWindow::saveOptions()
{
    if (!m_options->isValid())
        return;
    if (m_client->configureModel(m_selectedId, m_options->options(), m_options->explicitContent(), m_displayName->text()))
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

void ManageModelsWindow::editStyles()
{
    const auto open = [this] {
        for (const auto &model : m_client->models("image-generation")) {
            if (model.id != m_selectedId) continue;
            ModelStylesDialog dialog(m_client, model.id, model.styles, this);
            dialog.exec();
            break;
        }
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
    KiraUi::MainWindow::closeEvent(event);
}
