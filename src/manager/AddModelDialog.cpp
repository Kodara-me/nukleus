#include "AddModelDialog.h"
#include "ManagerServiceClient.h"
#include "ModelOptionsWidget.h"
#include <KiraUi/IconProvider.h>
#include <KiraUi/PrimaryButton.h>
#include <KiraUi/TitleBar.h>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
bool validUrl(const QString &text)
{
    const QUrl url(text.trimmed());
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           !url.hasFragment() && !url.fileName().isEmpty();
}
QString bytesText(qint64 bytes) { return QObject::tr("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1); }
} // namespace
AddModelDialog::AddModelDialog(ManagerServiceClient *client, const QString &category, QWidget *parent)
    : KiraUi::Dialog(parent), m_client(client)
{
    setWindowTitle(tr("Add model"));
    setWindowIcon(QIcon(QStringLiteral(":/manager/icons/manager-32.png")));
    titleBar()->setIcon(windowIcon());
    setModal(true);
    resize(620, 610);
    setMinimumSize(520, 480);
    auto *body = new QWidget(this);
    setContentWidget(body);
    auto *layout = new QVBoxLayout(body);
    auto *scroll = new QScrollArea(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *form = new QFormLayout(content);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_category = new QComboBox(content);
    m_category->setObjectName("modelCategory");
    for (const auto &item : QList<QPair<QString, QString>>{{tr("Image generation"), "image-generation"},
                                                           {tr("Edition"), "edition"},
                                                           {tr("Upscaler"), "upscaler"},
                                                           {tr("LLM"), "llm-chat"},
                                                           {tr("IP-Adapter"), "ip-adapter"}})
        m_category->addItem(item.first, item.second);
    if (!category.isEmpty())
        m_category->setCurrentIndex(m_category->findData(category));
    m_url = new QLineEdit(content);
    m_url->setObjectName("modelUrl");
    m_url->setPlaceholderText("https://example.com/model.gguf");
    m_companion = new QLineEdit(content);
    m_companion->setObjectName("companionUrl");
    m_companion->setPlaceholderText(tr("Optional HTTPS companion URL"));
    m_companionLabel = new QLabel(content);
    form->addRow(tr("Category:"), m_category);
    form->addRow(tr("Model URL:"), m_url);
    form->addRow(m_companionLabel, m_companion);
    auto *heading = new QLabel(tr("Recommended options"), content);
    form->addRow(heading);
    m_options = new ModelOptionsWidget(content);
    form->addRow(m_options);
    auto *hint =
        new QLabel(tr("Choose recommended values for this model. Applications can override them."), content);
    hint->setWordWrap(true);
    form->addRow(hint);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
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
    m_start = new KiraUi::PrimaryButton(tr("Start download"), body);
    m_start->setObjectName("startDownload");
    m_start->setIcon(KiraUi::IconProvider::icon(KiraUi::Icon::Plus));
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_start);
    layout->addLayout(buttons);
    connect(m_category, &QComboBox::currentIndexChanged, this, &AddModelDialog::updateCategory);
    connect(m_url, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_companion, &QLineEdit::textChanged, this, &AddModelDialog::updateActions);
    connect(m_options, &ModelOptionsWidget::optionsChanged, this, &AddModelDialog::updateActions);
    connect(m_start, &QPushButton::clicked, this, &AddModelDialog::startInstall);
    connect(m_cancel, &QPushButton::clicked, this, &AddModelDialog::reject);
    connect(m_client, &ManagerServiceClient::installsChanged, this, &AddModelDialog::updateProgress);
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this, [this] { updateActions(); });
    updateCategory();
}
void AddModelDialog::updateCategory()
{
    const QString category = m_category->currentData().toString();
    m_companion->clear();
    const bool supports = category != "upscaler";
    m_companion->setVisible(supports);
    m_companionLabel->setVisible(supports);
    m_companionLabel->setText(category == "llm-chat"     ? tr("Optional mmproj URL:")
                              : category == "ip-adapter" ? tr("Optional LoRA URL:")
                                                         : tr("Optional VAE URL:"));
    m_options->setOptions(category);
    updateActions();
}
void AddModelDialog::updateActions()
{
    m_category->setEnabled(!m_busy);
    m_url->setEnabled(!m_busy);
    m_companion->setEnabled(!m_busy);
    m_options->setEnabled(!m_busy);
    m_start->setEnabled(!m_busy && m_client->isAuthenticated() && validUrl(m_url->text()) &&
                        (m_category->currentData() == "upscaler" || m_companion->text().trimmed().isEmpty() ||
                         validUrl(m_companion->text())) &&
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
    m_installId = m_client->installModel(m_category->currentData().toString(), QUrl(m_url->text().trimmed()),
                                         QUrl(m_companion->text().trimmed()), m_options->options());
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
                    .arg(install.fileRole == "companion" ? tr("companion") : tr("model"),
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
