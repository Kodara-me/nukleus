#include "ModelRoutingDialog.h"
#include "ManagerServiceClient.h"
#include <KiraUi/PlainTextEdit.h>
#include <KiraUi/PrimaryButton.h>
#include <KiraUi/TreeView.h>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <kira/service/ModelOptions.h>

namespace
{
enum Kind
{
    Generation,
    Editing,
    Preset,
    Quality
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
    PermanentRole
};
QStandardItem *entry(const QString &name, Kind kind, const QString &id = {})
{
    auto *item = new QStandardItem(name);
    item->setData(kind, KindRole);
    item->setData(id, IdRole);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
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
    m_tree = new KiraUi::TreeView(splitter);
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
    const auto appendQuality = [this](QStandardItem *parentItem, const KiraService::ModelQuality &quality) {
        auto *item = entry(quality.name, Quality, quality.id);
        item->setData(quality.modelId, ModelRole);
        item->setData(quality.permanent, PermanentRole);
        auto steps = quality.steps;
        auto cfg = quality.cfgScale;
        if (!quality.modelId.isEmpty())
        {
            const auto defaults = KiraService::Protocol::defaultModelOptions("image-generation");
            QJsonObject options;
            for (const auto &model : m_client->models("image-generation"))
                if (model.id == quality.modelId)
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
        parentItem->appendRow(item);
    };
    for (const auto &preset : routing.presets)
    {
        auto *item = entry(preset.name, Preset, preset.id);
        item->setData(preset.positivePrefix, PositiveRole);
        item->setData(preset.negativePrefix, NegativeRole);
        m_generation->appendRow(item);
        for (const auto &quality : preset.qualities)
            appendQuality(item, quality);
    }
    for (const auto &quality : routing.editingQualities)
        appendQuality(m_editing, quality);
    m_tree->expandAll();

    m_editor = new QStackedWidget(splitter);
    auto *empty = new QLabel(tr("Select a preset to edit its prefixes, or a quality to choose its model."), m_editor);
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    m_editor->addWidget(empty);
    auto *prefixes = new QWidget(m_editor);
    auto *prefixLayout = new QVBoxLayout(prefixes);
    prefixLayout->setContentsMargins(0, 0, 0, 0);
    m_positive = new KiraUi::PlainTextEdit(prefixes);
    m_positive->setObjectName("presetPositive");
    m_negative = new KiraUi::PlainTextEdit(prefixes);
    m_negative->setObjectName("presetNegative");
    prefixLayout->addWidget(new QLabel(tr("Positive prefix"), prefixes));
    prefixLayout->addWidget(m_positive, 1);
    prefixLayout->addWidget(new QLabel(tr("Negative prefix"), prefixes));
    prefixLayout->addWidget(m_negative, 1);
    m_editor->addWidget(prefixes);
    auto *assignment = new QWidget(m_editor);
    auto *assignmentLayout = new QVBoxLayout(assignment);
    assignmentLayout->setContentsMargins(0, 0, 0, 0);
    m_assignment = new QComboBox(assignment);
    m_assignment->setObjectName("qualityModel");
    m_assignment->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_assignment->setMinimumContentsLength(12);
    auto *form = new QFormLayout;
    form->addRow(tr("Model:"), m_assignment);
    m_steps = new QSpinBox(assignment);
    m_steps->setObjectName("qualitySteps");
    m_steps->setRange(1, 200);
    m_cfg = new QDoubleSpinBox(assignment);
    m_cfg->setObjectName("qualityCfg");
    m_cfg->setRange(0, 100);
    m_cfg->setDecimals(4);
    m_cfg->setSingleStep(0.5);
    form->addRow(tr("Steps:"), m_steps);
    form->addRow(tr("CFG Scale:"), m_cfg);
    m_permanent = new QCheckBox(tr("Permanent quality"), assignment);
    m_permanent->setObjectName("qualityPermanent");
    m_permanent->setToolTip(tr("Use this quality in Canvas and hide the quality slider for this group."));
    form->addRow(m_permanent);
    assignmentLayout->addLayout(form);
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
            if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Preset)
                item->setData(m_positive->toPlainText(), PositiveRole);
    });
    connect(m_negative, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_showing)
            if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Preset)
                item->setData(m_negative->toPlainText(), NegativeRole);
    });
    connect(m_assignment, &QComboBox::currentIndexChanged, this, [this] {
        if (m_showing)
            return;
        auto *item = selectedItem();
        if (!item || item->data(KindRole).toInt() != Quality)
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
            if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Quality)
                item->setData(steps, StepsRole);
    });
    connect(m_cfg, &QDoubleSpinBox::valueChanged, this, [this](double cfg) {
        if (!m_showing)
            if (auto *item = selectedItem(); item && item->data(KindRole).toInt() == Quality)
                item->setData(cfg, CfgRole);
    });
    connect(m_cancel, &QPushButton::clicked, this, &ModelRoutingDialog::reject);
    connect(m_permanent, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_showing)
            return;
        auto *item = selectedItem();
        if (!item || item->data(KindRole).toInt() != Quality)
            return;
        if (checked)
            for (int row = 0; row < item->parent()->rowCount(); ++row)
                item->parent()->child(row)->setData(false, PermanentRole);
        item->setData(checked, PermanentRole);
    });
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
    m_editor->setCurrentIndex(kind == Preset ? 1 : kind == Quality ? 2 : 0);
    m_positive->setPlainText(kind == Preset ? item->data(PositiveRole).toString() : QString());
    m_negative->setPlainText(kind == Preset ? item->data(NegativeRole).toString() : QString());
    refreshModels();
    m_showing = true;
    const auto defaults = KiraService::Protocol::defaultModelOptions("image-generation");
    m_steps->setValue(kind == Quality && item->data(StepsRole).isValid() ? item->data(StepsRole).toInt()
                                                                         : defaults.value("steps").toInt());
    m_cfg->setValue(kind == Quality && item->data(CfgRole).isValid() ? item->data(CfgRole).toDouble()
                                                                     : defaults.value("cfgScale").toDouble());
    const bool assigned = kind == Quality && !item->data(ModelRole).toString().isEmpty();
    m_steps->setEnabled(assigned);
    m_cfg->setEnabled(assigned);
    m_permanent->setChecked(kind == Quality && item->data(PermanentRole).toBool());
    m_showing = false;
    updateActions();
}

void ModelRoutingDialog::refreshModels()
{
    m_showing = true;
    m_assignment->clear();
    m_assignment->addItem(tr("Unassigned"), QString());
    auto *item = selectedItem();
    if (item && item->data(KindRole).toInt() == Quality)
    {
        const bool editing = item->parent() == m_editing;
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

KiraService::ModelRouting ModelRoutingDialog::configuration() const
{
    auto routing = KiraService::Protocol::starterModelRouting();
    // Prompt flows have no editor yet; keep the service's saved flows unchanged.
    if (const auto saved = m_client->modelRouting())
        for (int row = 0; row < routing.presets.size() && row < saved->presets.size(); ++row)
            routing.presets[row].promptFlow = saved->presets.at(row).promptFlow;
    const auto qualities = [](QList<KiraService::ModelQuality> &result, const QStandardItem *parent) {
        for (int row = 0; row < result.size(); ++row)
        {
            const auto *item = parent->child(row);
            if (!item)
                continue;
            auto &q = result[row];
            q.modelId = item->data(ModelRole).toString();
            q.permanent = item->data(PermanentRole).toBool();
            if (item->data(StepsRole).isValid())
                q.steps = item->data(StepsRole).toInt();
            if (item->data(CfgRole).isValid())
                q.cfgScale = item->data(CfgRole).toDouble();
        }
    };
    for (int row = 0; row < routing.presets.size(); ++row)
    {
        const auto *item = m_generation->child(row);
        if (!item)
            continue;
        auto &preset = routing.presets[row];
        preset.positivePrefix = item->data(PositiveRole).toString();
        preset.negativePrefix = item->data(NegativeRole).toString();
        qualities(preset.qualities, item);
    }
    qualities(routing.editingQualities, m_editing);
    return routing;
}

void ModelRoutingDialog::updateActions()
{
    QString error;
    const bool valid = KiraService::Protocol::validateModelRouting(configuration(), &error);
    m_save->setEnabled(!m_saving && m_client->isAuthenticated() && m_client->supportsModelRouting() && valid);
    m_tree->setEnabled(!m_saving);
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
