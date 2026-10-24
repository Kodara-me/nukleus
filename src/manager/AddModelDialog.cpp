#include "AddModelDialog.h"
#include "ManagerServiceClient.h"
#include "ModelOptionsWidget.h"
#include <Ardoise/IconProvider.h>
#include <Ardoise/PrimaryButton.h>
#include <QCloseEvent>
#include <QComboBox>
#include <QSignalBlocker>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{
bool validUrl(const QString &text)
{
    const QUrl url(text.trimmed());
    if (url.isLocalFile()) {
        const QFileInfo file(url.toLocalFile());
        return url.host().isEmpty() && file.isFile() && file.isReadable();
    }
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           !url.hasFragment() && !url.fileName().isEmpty();
}
bool optionalUrl(const QLineEdit *field) { return field->text().trimmed().isEmpty() || validUrl(field->text()); }
QString bytesText(qint64 bytes) { return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1); }
} // namespace
AddModelDialog::AddModelDialog(ManagerServiceClient *client, const QString &category, QWidget *parent, const QString &existingModelId)
    : Ardoise::Dialog(parent), m_client(client), m_existingModelId(existingModelId)
{
    setWindowTitle(existingModelId.isEmpty() ? tr("Add model") : tr("Optional model files"));
    setWindowIcon(QIcon(QStringLiteral(":/manager/icons/manager-32.png")));
    setModal(true);
    resize(existingModelId.isEmpty() ? 960 : 620, 480);
    setMinimumSize(existingModelId.isEmpty() ? 900 : 520, 480);
    auto *body = new QWidget(this);
    setContentWidget(body);
    auto *layout = new QVBoxLayout(body);
    auto *columns = new QHBoxLayout;
    columns->setSpacing(24);
    auto *content = new QWidget(body);
    auto *form = new QFormLayout(content);
    m_form = form;
    form->setContentsMargins(0, 0, 0, 0);
    m_displayName = new QLineEdit(content);
    m_displayName->setObjectName("modelDisplayName");
    m_displayName->setMaxLength(256);
    m_displayName->setPlaceholderText(tr("Required display name"));
    if (m_existingModelId.isEmpty()) form->addRow(tr("Display name:"), m_displayName);
    else m_displayName->hide();
    const auto sourceRow = [this, content](QLineEdit* field) {
        auto* row = new QWidget(content);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(field, 1);
        auto* browse = new QPushButton(tr("Browse…"), row);
        browse->setObjectName(field->objectName() + "Browse");
        layout->addWidget(browse);
        auto* remove = new QPushButton(tr("Remove"), row);
        remove->setObjectName(field->objectName() + "Remove");
        remove->hide();
        layout->addWidget(remove);
        connect(remove, &QPushButton::clicked, this, [this, field] {
            const QString role = roleFor(field);
            if (QMessageBox::question(this, tr("Remove companion"),
                    tr("Remove the attached %1 file? The model and its other files will be kept.").arg(role)) != QMessageBox::Yes)
                return;
            m_pendingRemoval = role;
            if (!m_client->deleteCompanion(m_existingModelId, role)) {
                m_pendingRemoval.clear();
                m_status->setText(tr("Could not request companion removal."));
            }
            updateActions();
        });
        connect(browse, &QPushButton::clicked, this, [this, field] {
            const QString path = QFileDialog::getOpenFileName(this, tr("Choose model file"), {},
                tr("Model files (*.gguf *.safetensors *.ckpt *.bin *.onnx);;All files (*)"));
            if (!path.isEmpty()) field->setText(QUrl::fromLocalFile(path).toString());
        });
        return row;
    };
    const auto sourceField = [content](const QString &name, const QString &placeholder) {
        auto *field = new QLineEdit(content);
        field->setObjectName(name);
        field->setPlaceholderText(placeholder);
        return field;
    };
    form->setAlignment(Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_category = new QComboBox(content);
    m_category->setObjectName("modelCategory");
    for (const auto &item : QList<QPair<QString, QString>>{{tr("Image generation"), "image-generation"},
                                                           {tr("Animation"), "animation"},
                                                           {tr("Upscaler"), "upscaler"},
                                                           {tr("LLM"), "llm-chat"}, {tr("Companion"), "companion"}})
        m_category->addItem(item.first, item.second);
    if (!category.isEmpty())
        m_category->setCurrentIndex(m_category->findData(category));
    m_url = sourceField("modelUrl", "https://example.com/model.gguf");
    m_companion = sourceField("companionUrl", tr("Optional HTTPS companion URL"));
    m_highNoise = sourceField("highNoiseUrl", tr("Optional high-noise diffusion model"));
    m_loraLowNoise = sourceField("loraLowNoiseUrl", tr("Optional low-noise stage LoRA"));
    m_loraHighNoise = sourceField("loraHighNoiseUrl", tr("Optional high-noise stage LoRA"));
    m_newVae = sourceField("newVaeUrl", tr("URL or file of the VAE to install"));
    m_newTextEncoder = sourceField("newTextEncoderUrl", tr("URL or file of the T5-XXL text encoder to install"));
    m_companionLabel = new QLabel(content);
    m_mmprojSelection = new QComboBox(content);
    m_mmprojSelection->setObjectName("llmMmprojCompanion");
    m_mmprojSelectionLabel = new QLabel(tr("MMPROJ companion:"), content);
    form->addRow(m_mmprojSelectionLabel, m_mmprojSelection);
    connect(m_mmprojSelection, &QComboBox::currentIndexChanged, this, [this] {
        m_mmprojSelection->setProperty("selectedModelId", m_mmprojSelection->currentData());
    });
    connect(m_client, &ManagerServiceClient::modelsChanged, this, &AddModelDialog::refreshCompanions);
    form->addRow(tr("Category:"), m_category);
    m_urlLabel = new QLabel(tr("Model URL or file:"), content);
    form->addRow(m_urlLabel, sourceRow(m_url));
    form->addRow(m_companionLabel, sourceRow(m_companion));
    form->addRow(tr("High-noise model:"), sourceRow(m_highNoise));
    form->addRow(tr("Low-noise LoRA:"), sourceRow(m_loraLowNoise));
    form->addRow(tr("High-noise LoRA:"), sourceRow(m_loraHighNoise));
    form->addRow(tr("New VAE:"), sourceRow(m_newVae));
    form->addRow(tr("New text encoder:"), sourceRow(m_newTextEncoder));
    m_modelType = new QComboBox(content);
    m_modelType->setObjectName("modelType");
    m_modelType->addItem(tr("Complete checkpoint"), "checkpoint");
    m_modelType->addItem(tr("Standalone diffusion model (UNet)"), "diffusion");
    m_modelTypeLabel = new QLabel(tr("Model contents:"), content);
    form->addRow(m_modelTypeLabel, m_modelType);
    auto *recommendations = new QWidget(body);
    m_recommendations = recommendations;
    auto *optionsLayout = new QVBoxLayout(recommendations);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel(tr("Recommended options"), recommendations);
    optionsLayout->addWidget(heading);
    m_options = new ModelOptionsWidget(recommendations);
    m_options->setOffersNewCompanions(m_existingModelId.isEmpty());
    optionsLayout->addWidget(m_options);
    optionsLayout->addStretch();
    refreshCompanions();
    columns->addWidget(content, 1);
    columns->addWidget(recommendations, 1);
    layout->addLayout(columns, 1);
    m_status = new QLabel(body);
    m_status->setWordWrap(true);
    m_status->setObjectName("installStatus");
    layout->addWidget(m_status);
    m_progress = new QProgressBar(body);
    m_progress->hide();
    layout->addWidget(m_progress);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    m_cancel = new QPushButton(tr("Cancel"), body);
    m_start = new Ardoise::PrimaryButton(existingModelId.isEmpty() ? tr("Start download") : tr("Apply optional files"), body);
    m_start->setObjectName("startDownload");
    m_start->setIcon(Ardoise::IconProvider::icon(Ardoise::Icon::Plus));
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_start);
    layout->addLayout(buttons);
    connect(m_category, &QComboBox::currentIndexChanged, this, &AddModelDialog::updateCategory);
    connect(m_displayName, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    for (auto *field : {m_url, m_companion, m_highNoise, m_loraLowNoise, m_loraHighNoise, m_newVae, m_newTextEncoder})
        connect(field, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_options, &ModelOptionsWidget::optionsChanged, this, &AddModelDialog::updateActions);
    connect(m_start, &QPushButton::clicked, this, &AddModelDialog::startInstall);
    connect(m_cancel, &QPushButton::clicked, this, &AddModelDialog::reject);
    connect(m_client, &ManagerServiceClient::installsChanged, this, &AddModelDialog::updateProgress);
    connect(m_client, &ManagerServiceClient::companionDeleted, this, [this](const QString& id, const QString& role) {
        if (id != m_existingModelId) return;
        m_attachedFiles.remove(role);
        m_missingFiles.removeAll(role);
        m_pendingRemoval.clear();
        for (auto *field : QList<QLineEdit*>{m_companion} + animationFiles())
            if (roleFor(field) == role) { field->clear(); field->setPlaceholderText({}); }
        m_status->setText(tr("Companion removed. Choose a replacement file or URL."));
        updateActions();
    });
    connect(m_client, &ManagerServiceClient::modelOperationFailed, this, [this](const QString& id, const QString& message) {
        if (id != m_existingModelId || m_pendingRemoval.isEmpty()) return;
        m_pendingRemoval.clear(); m_status->setText(message); updateActions();
    });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this, [this] {
        if (!m_client->isAuthenticated()) m_pendingRemoval.clear();
        updateActions();
    });
    updateCategory();
    if (!m_existingModelId.isEmpty()) {
        recommendations->hide();
        for (const auto &model : m_client->models(category)) {
            if (model.id != m_existingModelId) continue;
            m_attachedFiles = model.companionFiles;
            m_missingFiles = model.missingFiles;
            if (!model.companionFileName.isEmpty()) m_attachedFiles.insert(model.companionRole, model.companionFileName);
            m_modelType->setCurrentIndex(m_modelType->findData(model.modelType));
        }
        for (auto *field : QList<QLineEdit*>{m_companion} + animationFiles()) {
            const QString role = roleFor(field);
            if (!m_attachedFiles.contains(role)) continue;
            const QString name = m_attachedFiles.value(role).toString().section('/', -1);
            field->setPlaceholderText(m_missingFiles.contains(role) ? tr("Missing: %1").arg(name)
                                                                    : tr("Already attached: %1").arg(name));
        }
        updateActions();
    }
}
bool AddModelDialog::isAnimation() const { return m_category->currentData().toString() == "animation"; }
QList<QLineEdit*> AddModelDialog::animationFiles() const { return {m_highNoise, m_loraLowNoise, m_loraHighNoise}; }
void AddModelDialog::showRow(QLineEdit *field, bool visible)
{
    field->parentWidget()->setVisible(visible);
    if (auto *label = m_form->labelForField(field->parentWidget())) label->setVisible(visible);
}
void AddModelDialog::updateCategory()
{
    const QString category = m_category->currentData().toString();
    m_companion->clear();
    const bool supports = category == "image-generation" && !m_existingModelId.isEmpty();
    m_companionLabel->setText(tr("Optional VAE URL:"));
    showRow(m_companion, supports);
    const bool animation = category == "animation";
    m_urlLabel->setText(animation ? tr("Diffusion model URL or file:") : tr("Model URL or file:"));
    for (auto *field : animationFiles()) {
        if (!animation) field->clear();
        showRow(field, animation);
    }
    m_mmprojSelection->setVisible(category == "llm-chat");
    m_mmprojSelectionLabel->setVisible(category == "llm-chat");
    m_recommendations->setVisible(category != "upscaler");
    const bool image = category == "image-generation";
    m_modelType->setVisible(image);
    m_modelTypeLabel->setVisible(image);
    m_modelType->setCurrentIndex(0);
    m_options->setOptions(category);
    m_options->setModels(m_client->models("companion") + m_client->models("llm-chat"));
    updateActions();
}
void AddModelDialog::updateActions()
{
    const bool busy = m_busy || !m_pendingRemoval.isEmpty();
    const bool adding = m_existingModelId.isEmpty();
    const bool animation = isAnimation();
    const QJsonObject options = m_options->options();
    const bool newVae = animation && adding && options.value("vaeModelId") == ModelOptionsWidget::newCompanionId();
    const bool newTextEncoder = animation && adding && options.value("textEncoderModelId") == ModelOptionsWidget::newCompanionId();
    showRow(m_newVae, newVae);
    showRow(m_newTextEncoder, newTextEncoder);
    m_category->setEnabled(!busy && adding);
    m_url->setEnabled(!busy);
    m_displayName->setEnabled(!busy);
    const auto attachable = [this](QLineEdit *field) {
        const QString role = roleFor(field);
        return !m_attachedFiles.contains(role) || m_missingFiles.contains(role);
    };
    for (auto *field : QList<QLineEdit*>{m_companion} + animationFiles())
        field->setEnabled(!busy && attachable(field));
    for (auto *field : {m_newVae, m_newTextEncoder}) field->setEnabled(!busy);
    for (auto* field : {m_url, m_companion, m_highNoise, m_loraLowNoise, m_loraHighNoise, m_newVae, m_newTextEncoder}) {
        field->parentWidget()->findChild<QPushButton*>(field->objectName() + "Browse")->setEnabled(field->isEnabled());
        auto* remove = field->parentWidget()->findChild<QPushButton*>(field->objectName() + "Remove");
        remove->setVisible(!adding && !roleFor(field).isEmpty() && m_attachedFiles.contains(roleFor(field)));
        remove->setEnabled(!busy && m_client->isAuthenticated());
    }
    showRow(m_url, adding);
    m_modelType->setEnabled(!busy);
    m_options->setEnabled(!busy);
    m_mmprojSelection->setEnabled(!busy);
    bool files = true;
    for (auto *field : QList<QLineEdit*>{m_companion} + animationFiles()) files = files && optionalUrl(field);
    bool anyFile = false;
    for (auto *field : animationFiles()) anyFile = anyFile || !field->text().trimmed().isEmpty();
    m_start->setEnabled(!busy && m_client->isAuthenticated() && files
        && (adding ? validUrl(m_url->text()) && !m_displayName->text().trimmed().isEmpty() && m_options->isValid()
                   : !animation || anyFile)
        && (!newVae || validUrl(m_newVae->text())) && (!newTextEncoder || validUrl(m_newTextEncoder->text())));
    m_cancel->setEnabled(!m_canceling);
}
void AddModelDialog::startInstall()
{
    m_pendingCompanions.clear();
    if (isAnimation() && m_existingModelId.isEmpty()) {
        // New shared companions are installed first, each in its own installation, then the model with their IDs.
        const QJsonObject options = m_options->options();
        for (const auto &key : {QString("vaeModelId"), QString("textEncoderModelId")})
            if (options.value(key) == ModelOptionsWidget::newCompanionId()) m_pendingCompanions.append(key);
    }
    m_step = 0;
    m_stepCount = m_pendingCompanions.size() + 1;
    m_busy = true;
    m_progress->show();
    m_progress->setRange(0, 0);
    startNextStep();
}
void AddModelDialog::startNextStep()
{
    m_installId.clear();
    m_installingCompanion.clear();
    ++m_step;
    m_status->setText(tr("Requesting installation…"));
    updateActions();
    const bool started = m_pendingCompanions.isEmpty() ? startModelInstall() : startCompanionInstall(m_pendingCompanions.first());
    if (!started)
    {
        m_busy = false;
        m_progress->hide();
        m_status->setText(tr("Could not start installation."));
        updateActions();
    }
    else
        updateProgress();
}
bool AddModelDialog::startCompanionInstall(const QString &key)
{
    const bool vae = key == "vaeModelId";
    const QUrl source((vae ? m_newVae : m_newTextEncoder)->text().trimmed());
    // A companion installed from the form is named after its file; it can be renamed in the model list.
    const QString name = QFileInfo(source.fileName()).completeBaseName();
    m_installingCompanion = key;
    m_installId = m_client->installModel("companion", source, {}, QJsonObject{{"subtype", vae ? "vae" : "t5xxl"}}, {},
        QStringLiteral("checkpoint"), {}, std::nullopt, name.isEmpty() ? source.fileName() : name);
    return !m_installId.isEmpty();
}
bool AddModelDialog::startModelInstall()
{
    const QString category = m_category->currentData().toString();
    const bool animation = category == "animation";
    QJsonObject files;
    if (animation)
        for (auto *field : animationFiles())
            if (!field->text().trimmed().isEmpty()) files.insert(roleFor(field), field->text().trimmed());
    const QString modelType = animation ? QStringLiteral("diffusion")
        : m_modelType->isHidden() ? QStringLiteral("checkpoint") : m_modelType->currentData().toString();
    const bool classified = category != "upscaler" && category != "companion";
    m_installId = m_client->installModel(category, QUrl(m_url->text().trimmed()),
        QUrl(m_companion->text().trimmed()), m_existingModelId.isEmpty() ? m_options->options() : QJsonObject{},
        files, modelType, m_existingModelId,
        m_existingModelId.isEmpty() && classified ? std::optional<bool>(m_options->explicitContent()) : std::nullopt,
        m_existingModelId.isEmpty() ? std::optional<QString>(m_displayName->text()) : std::nullopt,
        category == "llm-chat" ? std::optional<QString>(m_mmprojSelection->currentData().toString()) : std::nullopt);
    return !m_installId.isEmpty();
}
QString AddModelDialog::fileRoleLabel(const QString &role) const
{
    if (m_installingCompanion == "vaeModelId") return tr("VAE");
    if (m_installingCompanion == "textEncoderModelId") return tr("text encoder");
    if (role == "primary") return isAnimation() ? tr("diffusion model") : tr("model");
    if (role == "companion") return tr("companion");
    if (role == "high-noise") return tr("high-noise model");
    if (role == "lora-low-noise") return tr("low-noise LoRA");
    if (role == "lora-high-noise") return tr("high-noise LoRA");
    return role;
}
void AddModelDialog::updateProgress()
{
    for (const auto &install : m_client->installs())
    {
        if (install.installId != m_installId)
            continue;
        if (install.state == ManagedInstallState::Completed)
        {
            if (!m_installingCompanion.isEmpty())
            {
                // The companion is now installed: the model refers to it, and a retry will not install it again.
                const QString key = m_pendingCompanions.takeFirst();
                (key == "vaeModelId" ? m_newVae : m_newTextEncoder)->clear();
                m_options->selectModel(key, install.modelId);
                startNextStep();
                return;
            }
            m_busy = false;
            emit modelInstalled(install.modelId);
            accept();
            return;
        }
        if (install.state == ManagedInstallState::Canceled)
        {
            m_busy = false;
            m_canceling = false;
            Ardoise::Dialog::reject();
            return;
        }
        if (install.state == ManagedInstallState::Failed)
        {
            m_busy = false;
            m_canceling = false;
            m_progress->hide();
            m_status->setText(install.message);
            updateActions();
            return;
        }
        const QString step = m_stepCount > 1 ? tr("Step %1 of %2: ").arg(m_step).arg(m_stepCount) : QString();
        if (m_canceling)
            m_status->setText(tr("Canceling installation…"));
        else if (install.state == ManagedInstallState::Verifying)
            m_status->setText(step + tr("Finalizing installation…"));
        else if (install.state == ManagedInstallState::Downloading)
        {
            m_status->setText(step +
                tr("Downloading %1: %2%3")
                    .arg(fileRoleLabel(install.fileRole),
                         bytesText(install.bytesDownloaded),
                         install.totalBytes ? tr(" / %1").arg(bytesText(*install.totalBytes)) : QString()));
            if (install.totalBytes && *install.totalBytes > 0)
            {
                m_progress->setRange(0, 1000);
                m_progress->setValue(int(1000.0 * install.bytesDownloaded / *install.totalBytes));
            }
            else
                m_progress->setRange(0, 0);
        }
        else if (!step.isEmpty())
            m_status->setText(step + tr("Requesting installation…"));
        updateActions();
        return;
    }
}
void AddModelDialog::reject()
{
    if (!m_busy)
    {
        Ardoise::Dialog::reject();
        return;
    }
    if (!m_canceling && m_client->cancelInstall(m_installId))
    {
        m_canceling = true;
        m_status->setText(tr("Canceling installation…"));
        updateActions();
    }
}
void AddModelDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy)
    {
        event->ignore();
        reject();
    }
    else
        Ardoise::Dialog::closeEvent(event);
}

QString AddModelDialog::roleFor(QLineEdit* field) const
{
    if (field == m_companion) {
        const QString category = m_category->currentData().toString();
        return category == "llm-chat" ? "mmproj" : "vae";
    }
    if (field == m_highNoise) return QStringLiteral("high-noise");
    if (field == m_loraLowNoise) return QStringLiteral("lora-low-noise");
    if (field == m_loraHighNoise) return QStringLiteral("lora-high-noise");
    return {};
}

void AddModelDialog::refreshCompanions()
{
    if (m_options) m_options->setModels(m_client->models("companion") + m_client->models("llm-chat"));
    const QString selected = m_mmprojSelection->property("selectedModelId").toString();
    const QSignalBlocker blocker(m_mmprojSelection);
    m_mmprojSelection->clear();
    m_mmprojSelection->addItem(tr("None"), QString());
    for (const auto& model : m_client->models("companion"))
        if (model.recommendedOptions.value("subtype") == "mmproj")
            m_mmprojSelection->addItem(model.displayLabel(), model.id);
    m_mmprojSelection->setCurrentIndex(qMax(0, m_mmprojSelection->findData(selected)));
}
