#include "AddModelDialog.h"
#include "ManagerServiceClient.h"
#include "ModelOptionsWidget.h"
#include <KiraUi/IconProvider.h>
#include <KiraUi/PrimaryButton.h>
#include <QCloseEvent>
#include <QComboBox>
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
QString bytesText(qint64 bytes) { return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1); }
} // namespace
AddModelDialog::AddModelDialog(ManagerServiceClient *client, const QString &category, QWidget *parent, const QString &existingModelId)
    : KiraUi::Dialog(parent), m_client(client), m_existingModelId(existingModelId)
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
    form->setContentsMargins(0, 0, 0, 0);
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
                tr("Model files (*.gguf *.safetensors *.ckpt *.bin);;All files (*)"));
            if (!path.isEmpty()) field->setText(QUrl::fromLocalFile(path).toString());
        });
        return row;
    };
    form->setAlignment(Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_category = new QComboBox(content);
    m_category->setObjectName("modelCategory");
    for (const auto &item : QList<QPair<QString, QString>>{{tr("Image generation"), "image-generation"},
                                                           {tr("Edition"), "edition"},
                                                           {tr("Upscaler"), "upscaler"},
                                                           {tr("LLM"), "llm-chat"},
                                                           {tr("IP-Adapter"), "ip_adapter"},
                                                           {tr("CLIP Vision"), "clip_vision"},
                                                           {tr("PhotoMaker"), "photomaker"}})
        m_category->addItem(item.first, item.second);
    if (!category.isEmpty())
        m_category->setCurrentIndex(m_category->findData(category == "ip-adapter" ? "ip_adapter" : category));
    m_url = new QLineEdit(content);
    m_url->setObjectName("modelUrl");
    m_url->setPlaceholderText("https://example.com/model.gguf");
    m_companion = new QLineEdit(content);
    m_companion->setObjectName("companionUrl");
    m_companion->setPlaceholderText(tr("Optional HTTPS companion URL"));
    m_companionLabel = new QLabel(content);
    form->addRow(tr("Category:"), m_category);
    form->addRow(tr("Model URL or file:"), sourceRow(m_url));
    form->addRow(m_companionLabel, sourceRow(m_companion));
    m_clipL = new QLineEdit(content);
    m_clipL->setObjectName("clipLUrl");
    m_clipLLabel = new QLabel(tr("Optional CLIP-L URL:"), content);
    form->addRow(m_clipLLabel, sourceRow(m_clipL));
    m_clipG = new QLineEdit(content);
    m_clipG->setObjectName("clipGUrl");
    m_clipGLabel = new QLabel(tr("Optional CLIP-G URL:"), content);
    form->addRow(m_clipGLabel, sourceRow(m_clipG));
    m_textEncoder = new QLineEdit(content);
    m_textEncoder->setObjectName("textEncoderUrl");
    m_textEncoderLabel = new QLabel(tr("Optional text encoder URL:"), content);
    form->addRow(m_textEncoderLabel, sourceRow(m_textEncoder));
    m_mmproj = new QLineEdit(content);
    m_mmproj->setObjectName("mmprojUrl");
    m_mmprojLabel = new QLabel(tr("Optional mmproj URL:"), content);
    form->addRow(m_mmprojLabel, sourceRow(m_mmproj));
    m_modelType = new QComboBox(content);
    m_modelType->setObjectName("modelType");
    m_modelType->addItem(tr("Complete checkpoint"), "checkpoint");
    m_modelType->addItem(tr("Standalone diffusion model (UNet)"), "diffusion");
    m_modelTypeLabel = new QLabel(tr("Model contents:"), content);
    form->addRow(m_modelTypeLabel, m_modelType);
    auto *recommendations = new QWidget(body);
    auto *optionsLayout = new QVBoxLayout(recommendations);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel(tr("Recommended options"), recommendations);
    optionsLayout->addWidget(heading);
    m_options = new ModelOptionsWidget(recommendations);
    optionsLayout->addWidget(m_options);
    auto *hint =
        new QLabel(tr("Choose recommended values for this model. Applications can override them."), recommendations);
    hint->setWordWrap(true);
    optionsLayout->addWidget(hint);
    optionsLayout->addStretch();
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
    m_start = new KiraUi::PrimaryButton(existingModelId.isEmpty() ? tr("Start download") : tr("Apply optional files"), body);
    m_start->setObjectName("startDownload");
    m_start->setIcon(KiraUi::IconProvider::icon(KiraUi::Icon::Plus));
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_start);
    layout->addLayout(buttons);
    connect(m_category, &QComboBox::currentIndexChanged, this, &AddModelDialog::updateCategory);
    connect(m_url, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_companion, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_clipL, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_clipG, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_textEncoder, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_mmproj, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_options, &ModelOptionsWidget::optionsChanged, this, &AddModelDialog::updateActions);
    connect(m_start, &QPushButton::clicked, this, &AddModelDialog::startInstall);
    connect(m_cancel, &QPushButton::clicked, this, &AddModelDialog::reject);
    connect(m_client, &ManagerServiceClient::installsChanged, this, &AddModelDialog::updateProgress);
    connect(m_client, &ManagerServiceClient::companionDeleted, this, [this](const QString& id, const QString& role) {
        if (id != m_existingModelId) return;
        m_attachedFiles.remove(role);
        m_pendingRemoval.clear();
        for (auto* field : {m_companion, m_clipL, m_clipG, m_textEncoder, m_mmproj})
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
        m_url->hide();
        form->labelForField(m_url->parentWidget())->hide();
        recommendations->hide();
        for (const auto &model : m_client->models(category)) {
            if (model.id != m_existingModelId) continue;
            m_attachedFiles = model.companionFiles;
            if (!model.companionFileName.isEmpty()) m_attachedFiles.insert(model.companionRole, model.companionFileName);
            m_modelType->setCurrentIndex(m_modelType->findData(model.modelType));
        }
        const QList<QPair<QString, QLineEdit*>> fields{{roleFor(m_companion), m_companion}, {"clip-l", m_clipL}, {"clip-g", m_clipG}, {"llm", m_textEncoder}, {"mmproj", m_mmproj}};
        for (const auto &field : fields)
            if (m_attachedFiles.contains(field.first)) field.second->setPlaceholderText(tr("Already attached: %1").arg(m_attachedFiles.value(field.first).toString().section('/', -1)));
        updateActions();
    }
}
void AddModelDialog::updateCategory()
{
    const QString category = m_category->currentData().toString();
    m_companion->clear();
    const bool supports = category != "upscaler" && category != "clip_vision" && category != "photomaker";
    m_companion->setVisible(supports);
    m_companionLabel->setVisible(supports);
    m_companionLabel->setText(category == "llm-chat"     ? tr("Optional mmproj URL:")
                              : category == "ip_adapter" ? tr("Optional LoRA URL:")
                                                         : tr("Optional VAE URL:"));
    const bool image = category == "image-generation" || category == "edition";
    m_clipL->clear();
    m_clipG->clear();
    m_textEncoder->clear();
    m_mmproj->clear();
    for (QWidget *widget : QList<QWidget*>{m_clipL, m_clipG, m_clipLLabel, m_clipGLabel, m_textEncoder, m_mmproj, m_textEncoderLabel, m_mmprojLabel, m_modelType, m_modelTypeLabel}) widget->setVisible(image);
    for (auto* field : {m_clipL, m_clipG, m_textEncoder, m_mmproj}) field->parentWidget()->setVisible(image);
    m_companion->parentWidget()->setVisible(supports);
    m_modelType->setCurrentIndex(0);
    m_options->setOptions(category);
    updateActions();
}
void AddModelDialog::updateActions()
{
    const bool busy = m_busy || !m_pendingRemoval.isEmpty();
    m_category->setEnabled(!busy && m_existingModelId.isEmpty());
    m_url->setEnabled(!busy);
    m_companion->setEnabled(!busy && !m_attachedFiles.contains(roleFor(m_companion)));
    m_clipL->setEnabled(!busy && !m_attachedFiles.contains("clip-l"));
    m_clipG->setEnabled(!busy && !m_attachedFiles.contains("clip-g"));
    m_textEncoder->setEnabled(!busy && !m_attachedFiles.contains("llm"));
    m_mmproj->setEnabled(!busy && !m_attachedFiles.contains("mmproj"));
    for (auto* field : {m_url, m_companion, m_clipL, m_clipG, m_textEncoder, m_mmproj}) {
        field->parentWidget()->findChild<QPushButton*>(field->objectName() + "Browse")->setEnabled(field->isEnabled());
        auto* remove = field->parentWidget()->findChild<QPushButton*>(field->objectName() + "Remove");
        remove->setVisible(!m_existingModelId.isEmpty() && m_attachedFiles.contains(roleFor(field)));
        remove->setEnabled(!busy && m_client->isAuthenticated());
    }
    m_url->parentWidget()->setVisible(m_existingModelId.isEmpty());
    m_modelType->setEnabled(!busy);
    m_options->setEnabled(!busy);
    m_start->setEnabled(!busy && m_client->isAuthenticated() && (!m_existingModelId.isEmpty() || validUrl(m_url->text())) &&
                        (m_category->currentData() == "upscaler" || m_companion->text().trimmed().isEmpty() ||
                         validUrl(m_companion->text())) &&
                        (m_clipL->text().trimmed().isEmpty() || validUrl(m_clipL->text())) &&
                        (m_clipG->text().trimmed().isEmpty() || validUrl(m_clipG->text())) &&
                        (m_textEncoder->text().trimmed().isEmpty() || validUrl(m_textEncoder->text())) &&
                        (m_mmproj->text().trimmed().isEmpty() || validUrl(m_mmproj->text())) &&
                        m_options->isValid());
    m_cancel->setEnabled(!m_canceling);
}
void AddModelDialog::startInstall()
{
    m_installId.clear();
    m_busy = true;
    m_status->setText(tr("Requesting installation…"));
    m_progress->show();
    m_progress->setRange(0, 0);
    updateActions();
    QJsonObject files;
    if (!m_textEncoder->text().trimmed().isEmpty()) files.insert("llm", m_textEncoder->text().trimmed());
    if (!m_mmproj->text().trimmed().isEmpty()) files.insert("mmproj", m_mmproj->text().trimmed());
    if (!m_clipL->text().trimmed().isEmpty()) files.insert("clip-l", m_clipL->text().trimmed());
    if (!m_clipG->text().trimmed().isEmpty()) files.insert("clip-g", m_clipG->text().trimmed());
    m_installId = m_client->installModel(m_category->currentData().toString(), QUrl(m_url->text().trimmed()),
        QUrl(m_companion->text().trimmed()), m_existingModelId.isEmpty() ? m_options->options() : QJsonObject{},
        files, m_modelType->isHidden() ? QString("checkpoint") : m_modelType->currentData().toString(), m_existingModelId,
        m_existingModelId.isEmpty() ? std::optional<bool>(m_options->explicitContent()) : std::nullopt);
    if (m_installId.isEmpty())
    {
        m_busy = false;
        m_status->setText(tr("Could not start installation."));
        updateActions();
    }
    else
        updateProgress();
}
void AddModelDialog::updateProgress()
{
    for (const auto &install : m_client->installs())
    {
        if (install.installId != m_installId)
            continue;
        if (install.state == ManagedInstallState::Completed)
        {
            m_busy = false;
            emit modelInstalled(install.modelId);
            accept();
            return;
        }
        if (install.state == ManagedInstallState::Canceled)
        {
            m_busy = false;
            m_canceling = false;
            KiraUi::Dialog::reject();
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
        if (m_canceling)
            m_status->setText(tr("Canceling installation…"));
        else if (install.state == ManagedInstallState::Verifying)
            m_status->setText(tr("Finalizing installation…"));
        else if (install.state == ManagedInstallState::Downloading)
        {
            m_status->setText(
                tr("Downloading %1: %2%3")
                    .arg(install.fileRole == "primary" ? tr("model") : install.fileRole == "companion" ? tr("companion") : install.fileRole,
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
        updateActions();
        return;
    }
}
void AddModelDialog::reject()
{
    if (!m_busy)
    {
        KiraUi::Dialog::reject();
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
        KiraUi::Dialog::closeEvent(event);
}

QString AddModelDialog::roleFor(QLineEdit* field) const
{
    if (field == m_clipL) return "clip-l";
    if (field == m_clipG) return "clip-g";
    if (field == m_textEncoder) return "llm";
    if (field == m_mmproj) return "mmproj";
    if (field == m_companion) {
        const QString category = m_category->currentData().toString();
        return category == "llm-chat" ? "mmproj" : category == "ip_adapter" ? "lora" : "vae";
    }
    return {};
}
