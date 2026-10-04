#include "ModelOptionsWidget.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <kira/service/ModelOptions.h>

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
    m_explicitContent = new QCheckBox(tr("Explicit content (NSFW)"), this);
    m_explicitContent->setObjectName("explicitContent");
    m_explicitContent->setChecked(explicitContent);
    m_form->addRow(m_explicitContent);
    connect(m_explicitContent, &QCheckBox::toggled, this, &ModelOptionsWidget::optionsChanged);
    const auto values = saved.isEmpty() ? KiraService::Protocol::defaultModelOptions(category) : saved;
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
    if (category == "image-generation" || category == "edition")
    {
        auto *resolution = new QComboBox(this);
        for (int side = 256; side <= 2048; side += 256)
            resolution->addItem(tr("%1 × %1").arg(side), side);
        resolution->setCurrentIndex(resolution->findData(values.value("width").toInt()));
        connect(resolution, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add("resolution", tr("Resolution:"), resolution);
        integer("steps", tr("Steps:"), 1, 200);
        decimal("cfgScale", tr("CFG scale:"), 0, 100, 1);
        choices("sampler", tr("Sampler:"), KiraService::Protocol::modelSamplers());
        choices("scheduler", tr("Scheduler:"), KiraService::Protocol::modelSchedulers());
        auto *clip = new QComboBox(this);
        clip->addItem(tr("Automatic"), -1);
        for (int value = 1; value <= 12; ++value)
            clip->addItem(QString::number(value), value);
        clip->setCurrentIndex(clip->findData(values.value("clipSkip").toInt()));
        connect(clip, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add("clipSkip", tr("Clip skip:"), clip);
    }
    else if (category == "llm-chat")
    {
        decimal("temperature", tr("Temperature:"), 0, 2, 2);
        decimal("topP", tr("Top-p:"), 0.01, 1, 2);
        integer("contextLength", tr("Context length:"), 256, 131072);
        integer("maxTokens", tr("Maximum tokens:"), 1, 131072);
    }
    else if (category == "upscaler")
    {
        auto *tiles = new QComboBox(this);
        for (int side : {64, 128, 256, 512, 1024})
            tiles->addItem(QString::number(side), side);
        tiles->setCurrentIndex(tiles->findData(values.value("tileSize").toInt()));
        connect(tiles, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
        add("tileSize", tr("Tile size:"), tiles);
        m_form->addRow(new QLabel(tr("Upscaling remains fixed at 2×."), this));
    }
    else if (category == "ip-adapter")
        decimal("strength", tr("Strength:"), 0, 2, 2);
}
QJsonObject ModelOptionsWidget::options() const
{
    QJsonObject values;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
    {
        if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
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
    return KiraService::Protocol::validateModelOptions(m_category, options());
}

bool ModelOptionsWidget::explicitContent() const
{
    return m_explicitContent && m_explicitContent->isChecked();
}
