#include "ModelOptionsWidget.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
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
    m_animationFields.clear();
    m_animationError = nullptr;
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
    else if (category == "animation")
    {
        modelChoice("vaeModelId", tr("VAE:"));
        modelChoice("textEncoderModelId", tr("Text encoder (T5-XXL):"));
        addAnimationDefaults(saved);
    }
    else if (category == "companion") {
        auto* subtype = new QComboBox(this);
        subtype->addItem(tr("VAE"), "vae");
        subtype->addItem(tr("MMPROJ (vision projector)"), "mmproj");
        subtype->addItem(tr("T5-XXL (animation text encoder)"), "t5xxl");
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
        const bool animation = m_category == "animation";
        for (const auto &model : m_models)
            if ((key == "vaeModelId" && model.id.startsWith("companion:") && model.recommendedOptions.value("subtype") == "vae")
                || (key == "textEncoderModelId" && !animation && model.id.startsWith("llm-chat:") && model.recommendedOptions.value("canBeTextEncoder").toBool())
                || (key == "textEncoderModelId" && animation && model.id.startsWith("companion:") && model.recommendedOptions.value("subtype") == "t5xxl"))
                field->addItem(model.displayLabel(), model.id);
        if (animation && m_offersNewCompanions)
            field->addItem(tr("New from URL or file…"), newCompanionId());
        int index = field->findData(selected);
        if (index < 0 && !selected.isEmpty()) {
            field->addItem(tr("Unavailable: %1").arg(selected), selected);
            index = field->count() - 1;
        }
        field->setCurrentIndex(qMax(0, index));
    }
}
void ModelOptionsWidget::setOffersNewCompanions(bool offers)
{
    m_offersNewCompanions = offers;
    setModels(m_models);
}
void ModelOptionsWidget::selectModel(const QString &key, const QString &id)
{
    auto *field = qobject_cast<QComboBox*>(m_fields.value(key));
    if (!field) return;
    field->setProperty("selectedModelId", id);
    setModels(m_models);
    emit optionsChanged();
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
    if (m_category == "animation")
    {
        const auto parameters = animationParameters();
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            values.insert(it.key(), it.value());
    }
    return values;
}

namespace
{
using Nukleus::Protocol::AnimationOption;
using Kind = Nukleus::Protocol::AnimationOptionKind;

QString numberText(double value)
{
    return QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest);
}

// "videoFrames" -> "Video frames", with the acronyms sd-cli users know.
QString optionLabel(const QString &key)
{
    QString words;
    for (const QChar letter : key)
    {
        if (letter.isUpper() && !words.isEmpty())
            words += QLatin1Char(' ');
        words += letter.toLower();
    }
    // Acronyms are replaced while the words are still lowercase, so one that starts the label matches too.
    for (const auto &[word, acronym] : {std::pair{"Cfg", "CFG"}, std::pair{"Vae", "VAE"}, std::pair{"Slg", "SLG"},
                                        std::pair{"Rng", "RNG"}, std::pair{"Scm", "SCM"}, std::pair{"Lora", "LoRA"}})
        words.replace(QString::fromLatin1(word).toLower(), QString::fromLatin1(acronym));
    words[0] = words[0].toUpper();
    return words;
}

QString lowNoiseKey(const QString &key)
{
    QString low = key.mid(int(qstrlen("highNoise")));
    low[0] = low[0].toLower();
    return low;
}

QString listText(const QJsonValue &value)
{
    QStringList items;
    for (const auto &item : value.toArray())
        items.append(numberText(item.toDouble()));
    return items.join(QLatin1Char(','));
}

// The saved value of an option as the text of its field; empty when there is none.
QString fieldText(const AnimationOption &option, const QJsonValue &value)
{
    if (option.kind == Kind::IntegerList)
        return listText(value);
    return value.isDouble() ? numberText(value.toDouble()) : value.toString();
}

// What a text field holds, typed by its option. Text that does not read as its type stays a string, so the
// shared validation rejects it and names the option.
QJsonValue fieldValue(const AnimationOption &option, const QString &text)
{
    bool ok = false;
    switch (option.kind)
    {
    case Kind::Integer:
    {
        const qint64 whole = text.toLongLong(&ok);
        if (ok)
            return QJsonValue(whole);
        [[fallthrough]];
    }
    case Kind::Number:
    case Kind::NumberOrText:
    {
        const double number = text.toDouble(&ok);
        return ok ? QJsonValue(number) : QJsonValue(text);
    }
    case Kind::IntegerList:
    {
        QJsonArray list;
        for (const QString &item : text.split(QLatin1Char(',')))
        {
            const qint64 whole = item.trimmed().toLongLong(&ok);
            if (!ok)
                return QJsonValue(text);
            list.append(QJsonValue(whole));
        }
        return list;
    }
    default:
        return QJsonValue(text);
    }
}
} // namespace

void ModelOptionsWidget::addAnimationDefaults(const QJsonObject &values)
{
    const auto &table = Nukleus::Protocol::animationOptions();
    const auto find = [&table](const QString &key) -> const AnimationOption * {
        for (const auto &option : table)
            if (option.key == key)
                return &option;
        return nullptr;
    };
    const auto text = [this](const QString &key, const QString &saved, const QString &placeholder) {
        auto *field = new QLineEdit(saved, this);
        field->setObjectName(key);
        field->setPlaceholderText(placeholder);
        m_animationFields.insert(key, field);
        connect(field, &QLineEdit::textChanged, this, &ModelOptionsWidget::animationFieldChanged);
        return field;
    };
    // A combo box whose first entry is "no default"; booleans offer Yes and No.
    const auto choice = [this](const AnimationOption &option, const QJsonValue &saved) {
        auto *field = new QComboBox(this);
        field->setObjectName(option.key);
        field->addItem(tr("No default"), QVariant());
        if (option.kind == Kind::Flag || option.kind == Kind::Boolean)
        {
            field->addItem(tr("Yes"), true);
            field->addItem(tr("No"), false);
            if (saved.isBool())
                field->setCurrentIndex(saved.toBool() ? 1 : 2);
        }
        else
        {
            for (const QString &name : option.choices)
                field->addItem(name, name);
            if (saved.isString() && field->findData(saved.toString()) < 0)
                field->addItem(saved.toString(), saved.toString());
            field->setCurrentIndex(saved.isString() ? field->findData(saved.toString()) : 0);
        }
        m_animationFields.insert(option.key, field);
        connect(field, &QComboBox::currentIndexChanged, this, &ModelOptionsWidget::animationFieldChanged);
        return field;
    };
    const auto widgetFor = [&](const AnimationOption &option) -> QWidget * {
        const QJsonValue saved = values.value(option.key);
        QWidget *field = nullptr;
        if (option.kind == Kind::Flag || option.kind == Kind::Boolean || option.kind == Kind::Choice
            || option.kind == Kind::Container)
            field = choice(option, saved);
        else
            field = text(option.key, fieldText(option, saved), tr("No default"));
        if (!option.flag.isEmpty())
            field->setToolTip(option.flag);
        return field;
    };
    QString group;
    for (const auto &option : table)
    {
        // A high-noise option is shown beside its low-noise counterpart.
        if (option.key.startsWith("highNoise") && find(lowNoiseKey(option.key)))
            continue;
        if (option.group != group)
        {
            group = option.group;
            static const QHash<QString, const char *> groupNames{
                {"Video", QT_TR_NOOP("Video")},       {"Sampling", QT_TR_NOOP("Sampling")}, {"LoRA", QT_TR_NOOP("LoRA")},
                {"Cache", QT_TR_NOOP("Cache")},       {"Memory", QT_TR_NOOP("Memory")},     {"VAE tiling", QT_TR_NOOP("VAE tiling")}};
            const char *groupName = groupNames.value(group, nullptr);
            auto *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(groupName ? tr(groupName) : group), this);
            heading->setObjectName("group" + QString(group).remove(QLatin1Char(' ')));
            heading->setContentsMargins(0, 8, 0, 0);
            m_form->addRow(heading);
        }
        const AnimationOption *high = find("highNoise" + QString(option.key).replace(0, 1, option.key.at(0).toUpper()));
        if (option.kind == Kind::LoraWeights)
        {
            const auto weights = values.value(option.key).toObject();
            auto *row = new QWidget(this);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(new QLabel(tr("Low noise"), row));
            layout->addWidget(text("loraWeights.lowNoise", weights.contains("lowNoise") ? numberText(weights.value("lowNoise").toDouble()) : QString(), tr("No default")), 1);
            layout->addWidget(new QLabel(tr("High noise"), row));
            layout->addWidget(text("loraWeights.highNoise", weights.contains("highNoise") ? numberText(weights.value("highNoise").toDouble()) : QString(), tr("No default")), 1);
            m_form->addRow(tr("LoRA weights:"), row);
        }
        else if (high)
        {
            auto *row = new QWidget(this);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(new QLabel(tr("Low noise"), row));
            layout->addWidget(widgetFor(option), 1);
            layout->addWidget(new QLabel(tr("High noise"), row));
            layout->addWidget(widgetFor(*high), 1);
            m_form->addRow(tr("%1:").arg(optionLabel(option.key)), row);
        }
        else
            m_form->addRow(tr("%1:").arg(optionLabel(option.key)), widgetFor(option));
    }
    m_animationError = new QLabel(this);
    m_animationError->setObjectName("optionsError");
    m_animationError->setWordWrap(true);
    m_animationError->setTextFormat(Qt::PlainText);
    m_animationError->setStyleSheet("color: #d9534f;");
    m_form->addRow(m_animationError);
    // A saved default that is already invalid explains why Save is disabled, without an edit first.
    QString error;
    if (!Nukleus::Protocol::validateAnimationParameters(animationParameters(), &error))
        m_animationError->setText(error);
}

QJsonObject ModelOptionsWidget::animationParameters() const
{
    QJsonObject parameters;
    QJsonObject weights;
    for (auto it = m_animationFields.cbegin(); it != m_animationFields.cend(); ++it)
    {
        QJsonValue value;
        if (auto *combo = qobject_cast<QComboBox *>(it.value()))
        {
            if (combo->currentData().isValid() && !combo->currentData().isNull())
                value = QJsonValue::fromVariant(combo->currentData());
        }
        else if (auto *line = qobject_cast<QLineEdit *>(it.value()))
        {
            const QString content = line->text().trimmed();
            if (content.isEmpty())
                continue;
            if (it.key().startsWith("loraWeights."))
            {
                bool ok = false;
                const double weight = content.toDouble(&ok);
                weights.insert(it.key().mid(int(qstrlen("loraWeights."))), ok ? QJsonValue(weight) : QJsonValue(content));
                continue;
            }
            for (const auto &option : Nukleus::Protocol::animationOptions())
                if (option.key == it.key())
                    value = fieldValue(option, content);
        }
        if (!value.isUndefined() && !value.isNull())
            parameters.insert(it.key(), value);
    }
    if (!weights.isEmpty())
        parameters.insert("loraWeights", weights);
    return parameters;
}

void ModelOptionsWidget::animationFieldChanged()
{
    if (m_animationError)
    {
        QString error;
        m_animationError->setText(Nukleus::Protocol::validateAnimationParameters(animationParameters(), &error) ? QString() : error);
    }
    emit optionsChanged();
}
bool ModelOptionsWidget::isValid() const
{
    return Nukleus::Protocol::validateModelOptions(m_category, options());
}

bool ModelOptionsWidget::explicitContent() const
{
    return m_explicitContent && m_explicitContent->isChecked();
}
