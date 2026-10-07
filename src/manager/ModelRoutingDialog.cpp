#include "ModelRoutingDialog.h"
#include "ManagerServiceClient.h"
#include <KiraUi/IconButton.h>
#include <KiraUi/PlainTextEdit.h>
#include <KiraUi/PrimaryButton.h>
#include <KiraUi/TreeView.h>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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
    PromptFlow
};
constexpr int MaximumFlowSteps = 4;
enum Role
{
    KindRole = Qt::UserRole + 1,
    IdRole,
    PositiveRole,
    NegativeRole,
    ModelRole,
    StepsRole,
    CfgRole,
    FlowRole,
    IdentityViewsRole
};
QStandardItem *entry(const QString &name, Kind kind, const QString &id = {})
{
    auto *item = new QStandardItem(name);
    item->setData(kind, KindRole);
    item->setData(id, IdRole);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}
// Presets and the Editing node each carry one model assignment.
bool assignable(const QStandardItem *item)
{
    return item && (item->data(KindRole).toInt() == Preset || item->data(KindRole).toInt() == Editing);
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
        auto *item = entry(preset.name, Preset, preset.id);
        item->setData(preset.positivePrefix, PositiveRole);
        item->setData(preset.negativePrefix, NegativeRole);
        item->setData(preset.identityViews, IdentityViewsRole);
        setAssignment(item, preset.model);
        m_generation->appendRow(item);
        QJsonArray flow;
        for (const auto &step : preset.promptFlow)
            flow.append(QJsonObject{{"id", step.id},
                                    {"name", step.name},
                                    {"nameFr", step.nameFr},
                                    {"modelId", step.modelId},
                                    {"instructions", step.instructions}});
        auto *flowItem = entry({}, PromptFlow, preset.id);
        item->appendRow(flowItem);
        setFlow(flowItem, flow);
    }
    setAssignment(m_editing, routing.editing);
    m_tree->expandAll();

    m_editor = new QStackedWidget(splitter);
    auto *empty = new QLabel(
        tr("Select a preset or Editing to choose its model and settings, or a prompt flow to edit its steps."),
        m_editor);
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
    // Editing has no prefixes or identity views, so it only shows the model form.
    m_prefixes = new QWidget(assignment);
    auto *prefixLayout = new QVBoxLayout(m_prefixes);
    prefixLayout->setContentsMargins(0, 0, 0, 0);
    m_identityViews = new QComboBox(m_prefixes);
    m_identityViews->setObjectName("presetIdentityViews");
    m_identityViews->addItem(tr("1 portrait"), 1);
    m_identityViews->addItem(tr("2 portraits"), 2);
    m_identityViews->addItem(tr("4 portraits"), 4);
    auto *presetForm = new QFormLayout;
    presetForm->addRow(tr("Identity portraits:"), m_identityViews);
    prefixLayout->addLayout(presetForm);
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
    auto *flowPage = new QWidget(m_editor);
    flowPage->setObjectName("promptFlowPage");
    auto *flowLayout = new QVBoxLayout(flowPage);
    flowLayout->setContentsMargins(0, 0, 0, 0);
    auto *flowHint = new QLabel(tr("User prompt → each step in order → image generation. Every step receives and "
                                   "returns the positive and negative prompts as JSON."),
                                flowPage);
    flowHint->setWordWrap(true);
    flowLayout->addWidget(flowHint);
    auto *stepsRow = new QHBoxLayout;
    m_flowSteps = new QListWidget(flowPage);
    m_flowSteps->setObjectName("flowSteps");
    m_flowSteps->setMaximumHeight(140);
    stepsRow->addWidget(m_flowSteps, 1);
    auto *stepActions = new QVBoxLayout;
    const auto stepButton = [&](KiraUi::Icon icon, const QString &label, const char *name) {
        auto *button = new KiraUi::IconButton(icon, label, flowPage);
        button->setObjectName(name);
        stepActions->addWidget(button);
        return button;
    };
    m_addStep = stepButton(KiraUi::Icon::Plus, tr("Add step"), "addFlowStep");
    m_moveStepUp = stepButton(KiraUi::Icon::ChevronUp, tr("Move step up"), "moveFlowStepUp");
    m_moveStepDown = stepButton(KiraUi::Icon::ChevronDown, tr("Move step down"), "moveFlowStepDown");
    m_removeStep = stepButton(KiraUi::Icon::Close, tr("Remove step"), "removeFlowStep");
    stepActions->addStretch();
    stepsRow->addLayout(stepActions);
    flowLayout->addLayout(stepsRow);
    m_stepEditor = new QWidget(flowPage);
    auto *stepLayout = new QVBoxLayout(m_stepEditor);
    stepLayout->setContentsMargins(0, 0, 0, 0);
    auto *stepForm = new QFormLayout;
    m_stepName = new QLineEdit(m_stepEditor);
    m_stepName->setObjectName("flowStepName");
    m_stepNameFr = new QLineEdit(m_stepEditor);
    m_stepNameFr->setObjectName("flowStepNameFr");
    m_stepModel = new QComboBox(m_stepEditor);
    m_stepModel->setObjectName("flowStepModel");
    m_stepModel->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_stepModel->setMinimumContentsLength(12);
    stepForm->addRow(tr("Name (English):"), m_stepName);
    stepForm->addRow(tr("Name (French):"), m_stepNameFr);
    stepForm->addRow(tr("LLM:"), m_stepModel);
    stepLayout->addLayout(stepForm);
    m_stepInstructions = new KiraUi::PlainTextEdit(m_stepEditor);
    m_stepInstructions->setObjectName("flowStepInstructions");
    stepLayout->addWidget(new QLabel(tr("Instructions"), m_stepEditor));
    stepLayout->addWidget(m_stepInstructions, 1);
    flowLayout->addWidget(m_stepEditor, 1);
    m_editor->addWidget(flowPage);
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
    connect(m_flowSteps, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_showing)
            showFlow(row);
    });
    connect(m_stepName, &QLineEdit::textEdited, this, [this](const QString &text) { editStep("name", text); });
    connect(m_stepNameFr, &QLineEdit::textEdited, this, [this](const QString &text) { editStep("nameFr", text); });
    connect(m_stepModel, &QComboBox::currentIndexChanged, this,
            [this] { editStep("modelId", m_stepModel->currentData().toString()); });
    connect(m_stepInstructions, &QPlainTextEdit::textChanged, this,
            [this] { editStep("instructions", m_stepInstructions->toPlainText()); });
    connect(m_addStep, &QAbstractButton::clicked, this, [this] {
        auto *item = selectedItem();
        auto flow = item->data(FlowRole).toJsonArray();
        const int number = int(flow.size()) + 1;
        flow.append(QJsonObject{{"id", "step-" + QUuid::createUuid().toString(QUuid::Id128).left(12)},
                                {"name", QString("Step %1").arg(number)},
                                {"nameFr", QString("Étape %1").arg(number)},
                                {"modelId", QString()},
                                {"instructions", QString()}});
        setFlow(item, flow);
        showFlow(int(flow.size()) - 1);
    });
    const auto moveStep = [this](int offset) {
        auto *item = selectedItem();
        auto flow = item->data(FlowRole).toJsonArray();
        const int row = m_flowSteps->currentRow();
        const int target = row + offset;
        if (row < 0 || target < 0 || target >= flow.size())
            return;
        const auto step = flow.at(row);
        flow.removeAt(row);
        flow.insert(target, step);
        setFlow(item, flow);
        showFlow(target);
    };
    connect(m_moveStepUp, &QAbstractButton::clicked, this, [moveStep] { moveStep(-1); });
    connect(m_moveStepDown, &QAbstractButton::clicked, this, [moveStep] { moveStep(1); });
    connect(m_removeStep, &QAbstractButton::clicked, this, [this] {
        auto *item = selectedItem();
        auto flow = item->data(FlowRole).toJsonArray();
        const int row = m_flowSteps->currentRow();
        if (row < 0 || row >= flow.size())
            return;
        flow.removeAt(row);
        setFlow(item, flow);
        showFlow(qMin(row, int(flow.size()) - 1));
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
    m_editor->setCurrentIndex(model ? 1 : kind == PromptFlow ? 2 : 0);
    m_prefixes->setVisible(kind == Preset);
    m_flowSteps->clear(); // A newly selected flow starts at its first step.
    m_positive->setPlainText(kind == Preset ? item->data(PositiveRole).toString() : QString());
    m_negative->setPlainText(kind == Preset ? item->data(NegativeRole).toString() : QString());
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
        const bool editing = item == m_editing;
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
    if (item && item->data(KindRole).toInt() == PromptFlow)
        showFlow(m_flowSteps->currentRow());
}

void ModelRoutingDialog::setFlow(QStandardItem *item, const QJsonArray &flow)
{
    item->setText(flow.size() == 1 ? tr("Prompt flow (1 step)") : tr("Prompt flow (%1 steps)").arg(flow.size()));
    item->setData(flow, FlowRole);
}

QString ModelRoutingDialog::stepLabel(int row, const QJsonObject &step) const
{
    const auto id = step.value("modelId").toString();
    QString model = id.isEmpty() ? tr("Unassigned") : tr("Unavailable — %1").arg(id);
    for (const auto &info : m_client->models("llm-chat"))
        if (info.id == id)
            model = info.displayLabel();
    return QString("%1. %2 — %3").arg(row + 1).arg(step.value("name").toString(), model);
}

void ModelRoutingDialog::showFlow(int row)
{
    auto *item = selectedItem();
    if (!item || item->data(KindRole).toInt() != PromptFlow)
        return;
    const auto flow = item->data(FlowRole).toJsonArray();
    const int count = int(flow.size());
    if (row < 0 && count > 0)
        row = 0;
    row = qMin(row, count - 1);
    m_showing = true;
    m_flowSteps->clear();
    for (int i = 0; i < count; ++i)
        m_flowSteps->addItem(stepLabel(i, flow.at(i).toObject()));
    m_flowSteps->setCurrentRow(row);
    const auto step = row >= 0 ? flow.at(row).toObject() : QJsonObject{};
    m_stepName->setText(step.value("name").toString());
    m_stepNameFr->setText(step.value("nameFr").toString());
    m_stepInstructions->setPlainText(step.value("instructions").toString());
    m_stepModel->clear();
    m_stepModel->addItem(tr("Unassigned"), QString());
    for (const auto &model : m_client->models("llm-chat"))
        m_stepModel->addItem(model.displayLabel(), model.id);
    const auto id = step.value("modelId").toString();
    int index = m_stepModel->findData(id);
    if (index < 0)
    {
        m_stepModel->addItem(tr("Unavailable — %1").arg(id), id);
        index = m_stepModel->count() - 1;
    }
    m_stepModel->setCurrentIndex(index);
    m_stepEditor->setEnabled(row >= 0);
    m_addStep->setEnabled(count < MaximumFlowSteps);
    m_moveStepUp->setEnabled(row > 0);
    m_moveStepDown->setEnabled(row >= 0 && row < count - 1);
    m_removeStep->setEnabled(row >= 0);
    m_showing = false;
}

void ModelRoutingDialog::editStep(const QString &key, const QString &value)
{
    auto *item = selectedItem();
    const int row = m_flowSteps->currentRow();
    if (m_showing || !item || item->data(KindRole).toInt() != PromptFlow || row < 0)
        return;
    auto flow = item->data(FlowRole).toJsonArray();
    auto step = flow.at(row).toObject();
    step.insert(key, value);
    flow[row] = step;
    setFlow(item, flow);
    // Update the label in place so typing keeps focus in the editor.
    m_flowSteps->item(row)->setText(stepLabel(row, step));
}

KiraService::ModelRouting ModelRoutingDialog::configuration() const
{
    auto routing = KiraService::Protocol::starterModelRouting();
    const auto assignment = [](KiraService::ModelAssignment &a, const QStandardItem *item) {
        a.modelId = item->data(ModelRole).toString();
        if (item->data(StepsRole).isValid())
            a.steps = item->data(StepsRole).toInt();
        if (item->data(CfgRole).isValid())
            a.cfgScale = item->data(CfgRole).toDouble();
    };
    for (int row = 0; row < routing.presets.size(); ++row)
    {
        const auto *item = m_generation->child(row);
        if (!item)
            continue;
        auto &preset = routing.presets[row];
        preset.positivePrefix = item->data(PositiveRole).toString();
        preset.negativePrefix = item->data(NegativeRole).toString();
        preset.identityViews = item->data(IdentityViewsRole).toInt();
        assignment(preset.model, item);
        for (int child = 0; child < item->rowCount(); ++child)
            if (item->child(child)->data(KindRole).toInt() == PromptFlow)
                for (const auto &value : item->child(child)->data(FlowRole).toJsonArray())
                {
                    const auto step = value.toObject();
                    preset.promptFlow.append({step.value("id").toString(), step.value("name").toString(),
                                              step.value("nameFr").toString(), step.value("modelId").toString(),
                                              step.value("instructions").toString()});
                }
    }
    assignment(routing.editing, m_editing);
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
