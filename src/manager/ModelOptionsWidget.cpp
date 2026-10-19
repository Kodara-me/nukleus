#include "ModelOptionsWidget.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <nukleus/ModelOptions.h>

ModelOptionsWidget::ModelOptionsWidget(QWidget *parent) : QWidget(parent), m_form(new QFormLayout(this))
{
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
}
void ModelOptionsWidget::setOptions(const QString &category, const QJsonObject &saved, bool explicitContent)
{
    QSignalBlocker blocker(this);
    while (m_form->rowCount())
        m_form->removeRow(0);
    m_fields.clear();
    m_category = category;
    m_explicitContent = nullptr;
    if (category != "upscaler" && category != "companion") {
        m_explicitContent = new QCheckBox(tr("Explicit content (NSFW)"), this);
        m_explicitContent->setObjectName("explicitContent");
        m_explicitContent->setChecked(explicitContent);
        m_form->addRow(m_explicitContent);
        connect(m_explicitContent, &QCheckBox::toggled, this, &ModelOptionsWidget::optionsChanged);
    }
    const auto values = saved.isEmpty() ? Nukleus::Protocol::defaultModelOptions(category) : saved;
    const auto add = [this](const QString &key, const QString &label, QWidget *field)
    {
        field->setObjectName(key);
        m_fields.insert(key, field);
        m_form->addRow(label, field);
    };
    const auto integer = [&](const QString &key, const QString &label, int low, int high)
    {
        auto *field = new QSpinBox(this);
        field->setRange(low, high);
        field->setValue(values.value(key).toInt());
        connect(field, &QSpinBox::valueChanged, this, &ModelOptionsWidget::optionsChanged);
        add(key, label, field);
    };
    const auto decimal = [&](const QString &key, const QString &label, double low, double high, int precision)
    {
        auto *field = new QDoubleSpinBox(this);
        field->setRange(low, high);
        field->setDecimals(precision);
        field->setSingleStep(precision == 1 ? 0.1 : 0.01);
        field->setValue(values.value(key).toDouble());
        connect(field, &QDoubleSpinBox::valueChanged, this, &ModelOptionsWidget::optionsChanged);
        add(key, label, field);
    };
    const auto choices = [&](const QString &key, const QString &label, const QStringList &choices)
    {
        auto *field = new QComboBox(this);
        for (const QString &choice : choices)
            field->addItem(choice.isEmpty() ? tr("Automatic") : choice, choice);
        field->setCurrentIndex(field->findData(values.value(key).toString()));
        connect(field, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add(key, label, field);
    };
    const auto check = [&](const QString &key, const QString &label, bool fallback) {
        auto *field = new QCheckBox(label, this);
        field->setChecked(values.value(key).toBool(fallback));
        connect(field, &QCheckBox::toggled, this, &ModelOptionsWidget::optionsChanged);
        add(key, QString(), field);
    };
    const auto modelChoice = [&](const QString &key, const QString &label) {
        auto *field = new QComboBox(this);
        field->setProperty("selectedModelId", values.value(key).toString());
        connect(field, &QComboBox::currentIndexChanged, this, [this, field] {
            field->setProperty("selectedModelId", field->currentData());
            emit optionsChanged();
        });
        add(key, label, field);
    };
    if (category == "image-generation")
    {
        check("canEdit", tr("Can Edit (uses reference images)"), true);
        check("canGenerate", tr("Can Generate"), true);
        modelChoice("vaeModelId", tr("VAE:"));
        modelChoice("textEncoderModelId", tr("Text encoder:"));
        auto *resolution = new QComboBox(this);
        for (int side = 256; side <= 2048; side += 256)
            resolution->addItem(tr("%1 × %1").arg(side), side);
        resolution->setCurrentIndex(resolution->findData(values.value("width").toInt()));
        connect(resolution, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add("resolution", tr("Resolution:"), resolution);
        integer("steps", tr("Steps:"), 1, 200);
        decimal("cfgScale", tr("CFG scale:"), 0, 100, 1);
        choices("sampler", tr("Sampler:"), Nukleus::Protocol::modelSamplers());
        choices("scheduler", tr("Scheduler:"), Nukleus::Protocol::modelSchedulers());
        auto *format = new QComboBox(this);
        format->addItem(tr("Natural sentences (Flux)"), "natural");
        format->addItem(tr("Short phrases (SDXL)"), "phrases");
        format->addItem(tr("Tags (Illustrious)"), "tags");
        format->setCurrentIndex(format->findData(values.value("promptFormat").toString("phrases")));
        format->setToolTip(tr("How this model reads prompts; clients write prompts in this format."));
        connect(format, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add("promptFormat", tr("Prompt format:"), format);
    }
    else if (category == "llm-chat")
    {
        check("canBeTextEncoder", tr("Can be used as text encoder"), false);
        check("defaultPromptEnhancer", tr("Default prompt enhancer"), false);
        m_fields.value("defaultPromptEnhancer")->setToolTip(tr("Only one LLM is the default prompt enhancer; saving replaces the current one."));
        check("defaultPromptTranslator", tr("Default prompt translator"), false);
        m_fields.value("defaultPromptTranslator")->setToolTip(tr("Only one LLM is the default prompt translator; saving replaces the current one."));
        check("defaultImageDescriber", tr("Default image describer"), false);
        m_fields.value("defaultImageDescriber")->setToolTip(
            tr("Only one LLM is the default image describer; saving replaces the current one. It sees images only with an MMPROJ companion."));
        decimal("temperature", tr("Temperature:"), 0, 2, 2);
        decimal("topP", tr("Top-p:"), 0.01, 1, 2);
        integer("contextLength", tr("Context length:"), 256, 131072);
        integer("maxTokens", tr("Maximum tokens:"), 1, 131072);
    }
    else if (category == "companion") {
        auto* subtype = new QComboBox(this);
        subtype->addItem(tr("VAE"), "vae");
        subtype->addItem(tr("MMPROJ (vision projector)"), "mmproj");
        subtype->setCurrentIndex(subtype->findData(values.value("subtype").toString()));
        connect(subtype, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add("subtype", tr("Type:"), subtype);
    }
    setModels(m_models);
}
void ModelOptionsWidget::setModels(const QList<ManagedModelInfo>& models)
{
    m_models = models;
    for (const auto &key : {QString("vaeModelId"), QString("textEncoderModelId")}) {
        auto *field = qobject_cast<QComboBox*>(m_fields.value(key));
        if (!field) continue;
        const QSignalBlocker blocker(field);
        const auto selected = field->property("selectedModelId").toString();
        field->clear();
        field->addItem(tr("None"), QString());
        for (const auto &model : m_models)
            if ((key == "vaeModelId" && model.id.startsWith("companion:") && model.recommendedOptions.value("subtype") == "vae")
                || (key == "textEncoderModelId" && model.id.startsWith("llm-chat:") && model.recommendedOptions.value("canBeTextEncoder").toBool()))
                field->addItem(model.displayLabel(), model.id);
        int index = field->findData(selected);
        if (index < 0 && !selected.isEmpty()) {
            field->addItem(tr("Unavailable: %1").arg(selected), selected);
            index = field->count() - 1;
        }
        field->setCurrentIndex(qMax(0, index));
    }
}
QJsonObject ModelOptionsWidget::options() const
{
    QJsonObject values;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
    {
        if (auto *check = qobject_cast<QCheckBox *>(it.value()))
            values.insert(it.key(), check->isChecked());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
            values.insert(it.key(), spin->value());
        else if (auto *decimal = qobject_cast<QDoubleSpinBox *>(it.value()))
            values.insert(it.key(), decimal->value());
        else if (auto *combo = qobject_cast<QComboBox *>(it.value()))
        {
            if (it.key() == "resolution")
            {
                values.insert("width", combo->currentData().toInt());
                values.insert("height", combo->currentData().toInt());
            }
            else
                values.insert(it.key(), QJsonValue::fromVariant(combo->currentData()));
        }
    }
    return values;
}
bool ModelOptionsWidget::isValid() const
{
    return Nukleus::Protocol::validateModelOptions(m_category, options());
}

bool ModelOptionsWidget::explicitContent() const
{
    return m_explicitContent && m_explicitContent->isChecked();
}
