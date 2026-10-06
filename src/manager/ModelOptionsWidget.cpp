#include "ModelOptionsWidget.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QSpinBox>
#include <KiraUi/PlainTextEdit.h>
#include <QTimer>
#include <kira/service/ModelOptions.h>

ModelOptionsWidget::ModelOptionsWidget(QWidget *parent) : QWidget(parent), m_form(new QFormLayout(this))
{
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
}
void ModelOptionsWidget::setOptions(const QString &category, const QJsonObject &saved, bool explicitContent, bool canEdit)
{
    QSignalBlocker blocker(this);
    while (m_form->rowCount())
        m_form->removeRow(0);
    m_fields.clear();
    m_canEdit = nullptr;
    m_category = category;
    m_explicitContent = new QCheckBox(tr("Explicit content (NSFW)"), this);
    m_explicitContent->setObjectName("explicitContent");
    m_explicitContent->setChecked(explicitContent);
    m_form->addRow(m_explicitContent);
    connect(m_explicitContent, &QCheckBox::toggled, this, &ModelOptionsWidget::optionsChanged);
    if (category == "image-generation") {
        m_canEdit = new QCheckBox(tr("Can edit images"), this);
        m_canEdit->setObjectName("canEdit");
        m_canEdit->setChecked(canEdit);
        m_form->addRow(m_canEdit);
        connect(m_canEdit, &QCheckBox::toggled, this, &ModelOptionsWidget::optionsChanged);
    }
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
    const auto boolean = [&](const QString &key, const QString &label)
    {
        auto *field = new QCheckBox(this);
        field->setChecked(values.value(key).toBool());
        connect(field, &QCheckBox::toggled, this, &ModelOptionsWidget::optionsChanged);
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
    if (category == "face-parsing")
    {
        auto* classes = new QListWidget(this);
        classes->setFixedHeight(fontMetrics().lineSpacing() * 6);
        const QStringList labels{tr("Background"), tr("Skin"), tr("Nose"), tr("Glasses"),
                                 tr("Left eye"), tr("Right eye"), tr("Left eyebrow"), tr("Right eyebrow"),
                                 tr("Left ear"), tr("Right ear"), tr("Mouth"), tr("Upper lip"),
                                 tr("Lower lip"), tr("Hair"), tr("Hat"), tr("Earrings"),
                                 tr("Necklace"), tr("Neck"), tr("Clothing")};
        const auto names = KiraService::Protocol::faceParsingClasses();
        const auto included = values.value("includedClasses").toArray();
        for (int index = 0; index < names.size(); ++index) {
            auto* item = new QListWidgetItem(labels.at(index), classes);
            item->setData(Qt::UserRole, names.at(index));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(included.contains(names.at(index)) ? Qt::Checked : Qt::Unchecked);
        }
        connect(classes, &QListWidget::itemChanged, this, &ModelOptionsWidget::optionsChanged);
        add("includedClasses", tr("Included regions:"), classes);
        integer("dilatePixels", tr("Mask expansion (pixels):"), 0, 256);
        integer("featherPixels", tr("Soft edge (pixels):"), 0, 256);
        boolean("invertMask", tr("Invert mask:"));
        boolean("preserveAspectRatio", tr("Preserve aspect ratio:"));
        boolean("outputAtSourceResolution", tr("Use source resolution:"));
        choices("provider", tr("Execution provider:"), {"CPUExecutionProvider"});
    }
    else if (category == "image-generation" || category == "inpaint" || category == "edition")
    {
        if (category != "inpaint") {
            auto *resolution = new QComboBox(this);
            for (int side = 256; side <= 2048; side += 256)
                resolution->addItem(tr("%1 × %1").arg(side), side);
            resolution->setCurrentIndex(resolution->findData(values.value("width").toInt()));
            connect(resolution, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::optionsChanged);
            add("resolution", tr("Resolution:"), resolution);
        }
        integer("steps", tr("Steps:"), 1, 200);
        decimal("cfgScale", tr("CFG scale:"), 0, 100, 1);
        if (category == "inpaint") {
            decimal("strength", tr("Inpaint strength:"), 0, 1, 2);
            auto* strength = qobject_cast<QDoubleSpinBox*>(m_fields.value("strength"));
            strength->setSingleStep(0.05);
            strength->setValue(values.value("strength").toDouble(0.65));
        }
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
    else if (category == "prompt-enhancer")
    {
        choices("subtype", tr("Subtype:"), {"T5", "LLM", "VLM"});
        const auto subtype = values.value("subtype").toString();
        if (subtype == "T5") {
            integer("maxInputTokens", tr("Maximum input tokens:"), 1, 131072);
            integer("numBeams", tr("Beams:"), 1, 64);
            integer("noRepeatNgramSize", tr("No-repeat n-gram size:"), 0, 128);
        } else {
            integer("contextLength", tr("Context length:"), 256, 131072);
        }
        integer("maxOutputTokens", tr("Maximum output tokens:"), 1, 131072);
        decimal("repetitionPenalty", tr("Repetition penalty:"), 0.01, 10, 2);
        boolean("doSample", tr("Sampling:"));
        if (subtype != "T5" || values.value("doSample").toBool()) {
            decimal("temperature", tr("Temperature:"), 0, 2, 2);
            decimal("topP", tr("Top-p:"), 0.01, 1, 2);
            integer("topK", tr("Top-k:"), 0, 10000);
        }
        if (subtype == "VLM") boolean("visionInputEnabled", tr("Vision input enabled:"));
        const QString key = subtype == "T5" ? "instructionPrompt" : "systemPrompt";
        auto* prompt = new KiraUi::PlainTextEdit(this);
        prompt->setPlainText(values.value(key).toString());
        prompt->setFixedHeight(prompt->heightForLines(3));
        add(key, subtype == "T5" ? tr("Instruction prompt:") : tr("System prompt:"), prompt);
        connect(prompt, &QPlainTextEdit::textChanged, this, &ModelOptionsWidget::optionsChanged);
        // Rebuild after the emitting control's signal completes: removeRow deletes controls.
        auto* subtypeField = qobject_cast<QComboBox*>(m_fields.value("subtype"));
        connect(subtypeField, &QComboBox::currentIndexChanged,
            this, [this, subtypeField] {
                const auto subtype = options().value("subtype").toString();
                const bool explicitContent = this->explicitContent();
                QTimer::singleShot(0, subtypeField, [this, subtype, explicitContent] {
                    setOptions("prompt-enhancer", KiraService::Protocol::defaultPromptEnhancerOptions(subtype), explicitContent);
                    emit optionsChanged();
                });
            });
        if (subtype != "T5") {
            const auto samplingVisibility = [this](bool enabled) {
                for (const auto& key : {"temperature", "topP", "topK"}) {
                    auto* field = m_fields.value(key);
                    m_form->labelForField(field)->setVisible(enabled);
                    field->setVisible(enabled);
                }
            };
            samplingVisibility(values.value("doSample").toBool());
            connect(qobject_cast<QCheckBox*>(m_fields.value("doSample")), &QCheckBox::toggled,
                this, samplingVisibility);
        } else {
            auto* samplingField = qobject_cast<QCheckBox*>(m_fields.value("doSample"));
            connect(samplingField, &QCheckBox::toggled,
                this, [this, samplingField](bool sampling) {
                    auto values = KiraService::Protocol::defaultPromptEnhancerOptions("T5", sampling);
                    const auto current = options();
                    for (const auto& key : current.keys()) if (values.contains(key)) values.insert(key, current.value(key));
                    const bool explicitContent = this->explicitContent();
                    QTimer::singleShot(0, samplingField, [this, values, explicitContent] {
                        setOptions("prompt-enhancer", values, explicitContent);
                        emit optionsChanged();
                    });
                });
        }
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
    else if (category == "ip-adapter" || category == "ip_adapter")
    {
        decimal("strength", tr("Strength:"), 0, 2, 2);
    }
    else if (category == "clip_vision")
    {
        integer("image_size", tr("Image size:"), 1, 4096);
        boolean("normalize_input", tr("Normalize input:"));
    }
    else if (category == "photomaker")
    {
        integer("style_strength", tr("Style strength:"), 0, 100);
    }
}
QJsonObject ModelOptionsWidget::options() const
{
    QJsonObject values;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
    {
        if (auto* list = qobject_cast<QListWidget*>(it.value())) {
            QJsonArray included;
            for (int index = 0; index < list->count(); ++index)
                if (list->item(index)->checkState() == Qt::Checked)
                    included.append(list->item(index)->data(Qt::UserRole).toString());
            values.insert(it.key(), included);
        }
        else if (auto *check = qobject_cast<QCheckBox *>(it.value()))
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
        else if (auto* prompt = qobject_cast<QPlainTextEdit*>(it.value()))
            values.insert(it.key(), prompt->toPlainText());
    }
    return values;
}
bool ModelOptionsWidget::isValid() const
{
    return KiraService::Protocol::validateModelOptions(m_category, options());
}

bool ModelOptionsWidget::canEdit() const
{
    return m_canEdit && m_canEdit->isChecked();
}

bool ModelOptionsWidget::explicitContent() const
{
    return m_explicitContent && m_explicitContent->isChecked();
}
