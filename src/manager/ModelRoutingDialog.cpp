#include "ModelRoutingDialog.h"
#include "ManagerServiceClient.h"
#include <KiraUi/IconButton.h>
#include <KiraUi/PlainTextEdit.h>
#include <KiraUi/PrimaryButton.h>
#include <KiraUi/TreeView.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QUuid>
#include <QVBoxLayout>
#include <kira/service/ModelOptions.h>

namespace
{
enum Kind
{
    Generation,
    Editing,
    Preset,
    EditingQuality
};
enum Role
{
    KindRole = Qt::UserRole + 1,
    IdRole,
    PositiveRole,
    NegativeRole,
    ModelRole,
    StepsRole,
    CfgRole,
    IdentityViewsRole,
    NameFrRole,
    DefaultRole
};
QStandardItem *entry(const QString &name, Kind kind, const QString &id = {})
{
    auto *item = new QStandardItem(name);
    item->setData(kind, KindRole);
    item->setData(id, IdRole);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}
// Presets and editing qualities each carry one model assignment.
bool assignable(const QStandardItem *item)
{
    return item && (item->data(KindRole).toInt() == Preset || item->data(KindRole).toInt() == EditingQuality);
}
QStandardItem *selectedPreset(QStandardItem *item)
{
    return item && item->data(KindRole).toInt() == Preset ? item : nullptr;
}

} // namespace

ModelRoutingDialog::ModelRoutingDialog(ManagerServiceClient *client, QWidget *parent)
    : KiraUi::Dialog(parent), m_client(client)
{
    setWindowTitle(tr("Presets"));
    setObjectName("modelRoutingDialog");
    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    auto *splitter = new QSplitter(Qt::Horizontal, body);
    auto *treePane = new QWidget(splitter);
    auto *treeLayout = new QVBoxLayout(treePane);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    m_tree = new KiraUi::TreeView(treePane);
    treeLayout->addWidget(m_tree, 1);
    auto *presetActions = new QHBoxLayout;
    m_addPreset = new KiraUi::IconButton(KiraUi::Icon::Plus, tr("Add preset"), treePane);
    m_addPreset->setObjectName("addPreset");
    m_removePreset = new KiraUi::IconButton(KiraUi::Icon::Close, tr("Remove preset"), treePane);
    m_removePreset->setObjectName("removePreset");
    presetActions->addWidget(m_addPreset);
    presetActions->addWidget(m_removePreset);
    presetActions->addStretch();
    treeLayout->addLayout(presetActions);
    m_tree->setObjectName("routingTree");
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setDragDropMode(QAbstractItemView::NoDragDrop);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_model = new QStandardItemModel(this);
    m_tree->setModel(m_model);
    m_generation = entry(tr("Generation"), Generation);
    m_editing = entry(tr("Editing"), Editing);
    m_model->appendRow(m_generation);
    m_model->appendRow(m_editing);
    const auto routing = client->modelRouting().value_or(KiraService::Protocol::starterModelRouting());
    const auto setAssignment = [this](QStandardItem *item, const KiraService::ModelAssignment &assignment) {
        item->setData(assignment.modelId, ModelRole);
        auto steps = assignment.steps;
        auto cfg = assignment.cfgScale;
        if (!assignment.modelId.isEmpty())
        {
            const auto defaults = KiraService::Protocol::defaultModelOptions("image-generation");
            QJsonObject options;
            for (const auto &model : m_client->models("image-generation"))
                if (model.id == assignment.modelId)
                {
                    options = model.recommendedOptions;
                    break;
                }
            if (!steps)
                steps = options.value("steps").toInt(defaults.value("steps").toInt());
            if (!cfg)
                cfg = options.value("cfgScale").toDouble(defaults.value("cfgScale").toDouble());
        }
        if (steps)
            item->setData(*steps, StepsRole);
        if (cfg)
            item->setData(*cfg, CfgRole);
    };
    for (const auto &preset : routing.presets)
    {
        auto *item = addPresetItem(preset);
        setAssignment(item, preset.model);
    }
    for (const auto &quality : routing.editingQualities)
    {
        auto *item = entry(quality.name, EditingQuality, quality.id);
        item->setData(quality.nameFr, NameFrRole);
        item->setData(quality.isDefault, DefaultRole);
        item->setData(quality.positivePrefix, PositiveRole);
        item->setData(quality.negativePrefix, NegativeRole);
        setAssignment(item, quality.model);
        m_editing->appendRow(item);
    }
    m_tree->expandAll();

    m_editor = new QStackedWidget(splitter);
    auto *empty = new QLabel(tr("Select a preset or an editing quality to choose its model and settings."), m_editor);
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    m_editor->addWidget(empty);
    auto *assignment = new QWidget(m_editor);
    auto *assignmentLayout = new QVBoxLayout(assignment);
    assignmentLayout->setContentsMargins(0, 0, 0, 0);
    m_assignment = new QComboBox(assignment);
    m_assignment->setObjectName("routingModel");
    m_assignment->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_assignment->setMinimumContentsLength(12);
    auto *form = new QFormLayout;
    form->addRow(tr("Model:"), m_assignment);
    m_steps = new QSpinBox(assignment);
    m_steps->setObjectName("routingSteps");
    m_steps->setRange(1, 200);
    m_cfg = new QDoubleSpinBox(assignment);
    m_cfg->setObjectName("routingCfg");
    m_cfg->setRange(0, 100);
    m_cfg->setDecimals(4);
    m_cfg->setSingleStep(0.5);
    form->addRow(tr("Steps:"), m_steps);
    form->addRow(tr("CFG Scale:"), m_cfg);
    assignmentLayout->addLayout(form);
    m_defaultQuality = new QCheckBox(tr("Default quality"), assignment);
    m_defaultQuality->setObjectName("editingDefaultQuality");
    m_defaultQuality->setToolTip(tr("Selected first in Canvas and used by features that edit without a quality "
                                    "choice, such as applying an identity."));
    assignmentLayout->addWidget(m_defaultQuality);
    // Presets and editing qualities both have prefixes; only presets have names and identity views.
    m_prefixes = new QWidget(assignment);
    auto *prefixLayout = new QVBoxLayout(m_prefixes);
    prefixLayout->setContentsMargins(0, 0, 0, 0);
    m_presetFields = new QWidget(m_prefixes);
    m_identityViews = new QComboBox(m_presetFields);
    m_identityViews->setObjectName("presetIdentityViews");
    m_identityViews->addItem(tr("1 portrait"), 1);
    m_identityViews->addItem(tr("2 portraits"), 2);
    m_identityViews->addItem(tr("4 portraits"), 4);
    m_presetName = new QLineEdit(m_presetFields);
    m_presetName->setObjectName("presetName");
    m_presetNameFr = new QLineEdit(m_presetFields);
    m_presetNameFr->setObjectName("presetNameFr");
    auto *presetForm = new QFormLayout(m_presetFields);
    presetForm->setContentsMargins(0, 0, 0, 0);
    presetForm->addRow(tr("Name (English):"), m_presetName);
    presetForm->addRow(tr("Name (French):"), m_presetNameFr);
    presetForm->addRow(tr("Identity portraits:"), m_identityViews);
    prefixLayout->addWidget(m_presetFields);
    m_positive = new KiraUi::PlainTextEdit(m_prefixes);
    m_positive->setObjectName("presetPositive");
    m_negative = new KiraUi::PlainTextEdit(m_prefixes);
    m_negative->setObjectName("presetNegative");
    prefixLayout->addWidget(new QLabel(tr("Positive prefix"), m_prefixes));
    prefixLayout->addWidget(m_positive, 1);
    prefixLayout->addWidget(new QLabel(tr("Negative prefix"), m_prefixes));
    prefixLayout->addWidget(m_negative, 1);
    assignmentLayout->addWidget(m_prefixes, 1);
    assignmentLayout->addStretch();
    m_editor->addWidget(assignment);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    m_status = new QLabel(body);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    m_save = new KiraUi::PrimaryButton(tr("Save"), body);
    m_save->setObjectName("saveRouting");
    m_cancel = new QPushButton(tr("Cancel"), body);
    actions->addWidget(m_save);
    actions->addWidget(m_cancel);
    layout->addLayout(actions);
    setContentWidget(body);
    resize(900, 620);

    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { showSelection(); });
    connect(m_model, &QStandardItemModel::dataChanged, this, [this] { updateActions(); });
    connect(m_model, &QStandardItemModel::rowsInserted, this, [this] { updateActions(); });
    connect(m_model, &QStandardItemModel::rowsRemoved, this, [this] { updateActions(); });
    connect(m_positive, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_showing)
            if (auto *item = selectedItem(); assignable(item))
                item->setData(m_positive->toPlainText(), PositiveRole);
    });
    connect(m_negative, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_showing)
            if (auto *item = selectedItem(); assignable(item))
                item->setData(m_negative->toPlainText(), NegativeRole);
    });
    connect(m_presetName, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Preset)
            item->setText(text);
    });
    connect(m_presetNameFr, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Preset)
            item->setData(text, NameFrRole);
    });
    connect(m_addPreset, &QAbstractButton::clicked, this, [this] {
        KiraService::GenerationPreset preset{"preset-" + QUuid::createUuid().toString(QUuid::Id128).left(12),
                                             tr("New preset"), {}, {}, {}};
        preset.nameFr = QStringLiteral("Nouveau préréglage");
        auto *item = addPresetItem(preset);
        m_tree->expand(item->index());
        m_tree->setCurrentIndex(item->index());
        m_presetName->setFocus();
        m_presetName->selectAll();
    });
    connect(m_removePreset, &QAbstractButton::clicked, this, [this] {
        auto *preset = selectedPreset(selectedItem());
        if (!preset || m_generation->rowCount() <= 1)
            return;
        const int row = preset->row();
        m_generation->removeRow(row);
        m_tree->setCurrentIndex(m_generation->child(qMin(row, m_generation->rowCount() - 1))->index());
    });
    connect(m_defaultQuality, &QCheckBox::toggled, this, [this](bool checked) {
        auto *item = selectedItem();
        if (m_showing || !checked || !item || item->data(KindRole).toInt() != EditingQuality)
            return;
        for (int row = 0; row < m_editing->rowCount(); ++row)
            m_editing->child(row)->setData(m_editing->child(row) == item, DefaultRole);
        m_defaultQuality->setEnabled(false);
    });
    connect(m_identityViews, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_showing)
            if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Preset)
                item->setData(m_identityViews->currentData(), IdentityViewsRole);
    });
    connect(m_assignment, &QComboBox::currentIndexChanged, this, [this] {
        if (m_showing)
            return;
        auto *item = selectedItem();
        if (!assignable(item))
            return;
        const auto id = m_assignment->currentData().toString();
        item->setData(id, ModelRole);
        const auto defaults = KiraService::Protocol::defaultModelOptions("image-generation");
        QJsonObject options;
        for (const auto &model : m_client->models("image-generation"))
            if (model.id == id)
            {
                options = model.recommendedOptions;
                break;
            }
        const int steps = options.value("steps").toInt(defaults.value("steps").toInt());
        const double cfg = options.value("cfgScale").toDouble(defaults.value("cfgScale").toDouble());
        m_showing = true;
        m_steps->setValue(steps);
        m_cfg->setValue(cfg);
        m_steps->setEnabled(!id.isEmpty());
        m_cfg->setEnabled(!id.isEmpty());
        m_showing = false;
        item->setData(id.isEmpty() ? QVariant() : QVariant(steps), StepsRole);
        item->setData(id.isEmpty() ? QVariant() : QVariant(cfg), CfgRole);
    });
    connect(m_steps, &QSpinBox::valueChanged, this, [this](int steps) {
        if (!m_showing)
            if (auto *item = selectedItem(); assignable(item))
                item->setData(steps, StepsRole);
    });
    connect(m_cfg, &QDoubleSpinBox::valueChanged, this, [this](double cfg) {
        if (!m_showing)
            if (auto *item = selectedItem(); assignable(item))
                item->setData(cfg, CfgRole);
    });
    connect(m_cancel, &QPushButton::clicked, this, &ModelRoutingDialog::reject);
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (!m_client->configureModelRouting(configuration()))
        {
            m_status->setText(tr("Could not submit configuration."));
            return;
        }
        m_saving = true;
        m_status->setText(tr("Saving…"));
        updateActions();
    });
    connect(m_client, &ManagerServiceClient::modelRoutingConfigured, this, [this] {
        if (m_saving)
            accept();
    });
    connect(m_client, &ManagerServiceClient::modelRoutingFailed, this, [this](const QString &error) {
        m_saving = false;
        updateActions();
        m_status->setText(error);
    });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this, [this] {
        if (!m_client->isAuthenticated())
            m_saving = false;
        updateActions();
        if (!m_client->isAuthenticated())
            m_status->setText(tr("Service disconnected; draft retained."));
    });
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &ModelRoutingDialog::refreshModels);
    m_tree->setCurrentIndex(m_generation->rowCount() ? m_generation->child(0)->index() : m_generation->index());
    showSelection();
}

QStandardItem *ModelRoutingDialog::selectedItem() const
{
    return m_model->itemFromIndex(m_tree->currentIndex());
}

void ModelRoutingDialog::showSelection()
{
    m_showing = true;
    auto *item = selectedItem();
    const int kind = item ? item->data(KindRole).toInt() : -1;
    const bool model = assignable(item);
    m_editor->setCurrentIndex(model ? 1 : 0);
    m_presetFields->setVisible(kind == Preset);
    m_defaultQuality->setVisible(kind == EditingQuality);
    m_defaultQuality->setChecked(kind == EditingQuality && item->data(DefaultRole).toBool());
    // Exactly one quality is the default: choosing another one moves it.
    m_defaultQuality->setEnabled(!m_defaultQuality->isChecked());
    m_presetName->setText(kind == Preset ? item->text() : QString());
    m_presetNameFr->setText(kind == Preset ? item->data(NameFrRole).toString() : QString());
    m_positive->setPlainText(model ? item->data(PositiveRole).toString() : QString());
    m_negative->setPlainText(model ? item->data(NegativeRole).toString() : QString());
    m_identityViews->setCurrentIndex(
        qMax(0, m_identityViews->findData(kind == Preset ? item->data(IdentityViewsRole).toInt() : 1)));
    refreshModels();
    m_showing = true;
    const auto defaults = KiraService::Protocol::defaultModelOptions("image-generation");
    m_steps->setValue(model && item->data(StepsRole).isValid() ? item->data(StepsRole).toInt()
                                                               : defaults.value("steps").toInt());
    m_cfg->setValue(model && item->data(CfgRole).isValid() ? item->data(CfgRole).toDouble()
                                                           : defaults.value("cfgScale").toDouble());
    const bool assigned = model && !item->data(ModelRole).toString().isEmpty();
    m_steps->setEnabled(assigned);
    m_cfg->setEnabled(assigned);
    m_showing = false;
    updateActions();
}

void ModelRoutingDialog::refreshModels()
{
    m_showing = true;
    m_assignment->clear();
    m_assignment->addItem(tr("Unassigned"), QString());
    auto *item = selectedItem();
    if (assignable(item))
    {
        const bool editing = item->data(KindRole).toInt() == EditingQuality;
        for (const auto &model : m_client->models("image-generation"))
            if (model.recommendedOptions.value(editing ? "canEdit" : "canGenerate").toBool(true))
                m_assignment->addItem(model.displayLabel(), model.id);
        const auto id = item->data(ModelRole).toString();
        int index = m_assignment->findData(id);
        if (index < 0)
        {
            m_assignment->addItem(tr("Unavailable — %1").arg(id), id);
            index = m_assignment->count() - 1;
        }
        m_assignment->setCurrentIndex(index);
    }
    m_showing = false;
}

QStandardItem *ModelRoutingDialog::addPresetItem(const KiraService::GenerationPreset &preset)
{
    auto *item = entry(preset.name, Preset, preset.id);
    item->setData(preset.nameFr, NameFrRole);
    item->setData(preset.positivePrefix, PositiveRole);
    item->setData(preset.negativePrefix, NegativeRole);
    item->setData(preset.identityViews, IdentityViewsRole);
    item->setData(preset.model.modelId, ModelRole);
    m_generation->appendRow(item);
    return item;
}

KiraService::ModelRouting ModelRoutingDialog::configuration() const
{
    KiraService::ModelRouting routing;
    const auto assignment = [](KiraService::ModelAssignment &a, const QStandardItem *item) {
        a.modelId = item->data(ModelRole).toString();
        if (item->data(StepsRole).isValid())
            a.steps = item->data(StepsRole).toInt();
        if (item->data(CfgRole).isValid())
            a.cfgScale = item->data(CfgRole).toDouble();
    };
    for (int row = 0; row < m_generation->rowCount(); ++row)
    {
        const auto *item = m_generation->child(row);
        auto &preset = routing.presets.emplace_back();
        preset.id = item->data(IdRole).toString();
        preset.name = item->text();
        preset.nameFr = item->data(NameFrRole).toString();
        preset.positivePrefix = item->data(PositiveRole).toString();
        preset.negativePrefix = item->data(NegativeRole).toString();
        preset.identityViews = item->data(IdentityViewsRole).toInt();
        assignment(preset.model, item);
    }
    for (int row = 0; row < m_editing->rowCount(); ++row)
    {
        const auto *item = m_editing->child(row);
        auto &quality = routing.editingQualities.emplace_back();
        quality.id = item->data(IdRole).toString();
        quality.name = item->text();
        quality.nameFr = item->data(NameFrRole).toString();
        quality.isDefault = item->data(DefaultRole).toBool();
        quality.positivePrefix = item->data(PositiveRole).toString();
        quality.negativePrefix = item->data(NegativeRole).toString();
        assignment(quality.model, item);
    }
    return routing;
}

void ModelRoutingDialog::updateActions()
{
    QString error;
    const bool valid = KiraService::Protocol::validateModelRouting(configuration(), &error);
    m_save->setEnabled(!m_saving && m_client->isAuthenticated() && m_client->supportsModelRouting() && valid);
    m_tree->setEnabled(!m_saving);
    m_addPreset->setEnabled(!m_saving &&
                            m_generation->rowCount() < KiraService::Protocol::MaximumGenerationPresets);
    m_removePreset->setEnabled(!m_saving && selectedPreset(selectedItem()) && m_generation->rowCount() > 1);
    m_editor->setEnabled(!m_saving);
    m_cancel->setEnabled(!m_saving);
    if (!m_saving)
        m_status->setText(error);
}

void ModelRoutingDialog::reject()
{
    if (!m_saving)
        KiraUi::Dialog::reject();
}
