#include "ModelStylesDialog.h"
#include "ManagerServiceClient.h"
#include <KiraUi/Panel.h>
#include <KiraUi/PrimaryButton.h>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>

ModelStylesDialog::ModelStylesDialog(ManagerServiceClient *client, const QString &modelId,
    const QList<KiraService::PromptStyle> &styles, QWidget *parent)
    : KiraUi::Dialog(parent), m_client(client), m_modelId(modelId), m_styles(styles)
{
    setWindowTitle(tr("Model styles"));
    resize(760, 480);
    auto *body = new QWidget(this);
    setContentWidget(body);
    auto *layout = new QVBoxLayout(body);
    auto *columns = new QHBoxLayout;
    auto *library = new KiraUi::Panel(body);
    auto *left = new QVBoxLayout(library);
    m_list = new QListWidget(library);
    m_list->setObjectName("stylesList");
    for (const auto &style : styles) m_list->addItem(style.name);
    left->addWidget(m_list);
    auto *actions = new QHBoxLayout;
    m_add = new QPushButton(tr("Add"), library); m_add->setObjectName("addStyle");
    m_remove = new QPushButton(tr("Remove"), library); m_remove->setObjectName("removeStyle");
    actions->addWidget(m_add); actions->addWidget(m_remove); left->addLayout(actions);
    columns->addWidget(library, 1);
    m_form = new KiraUi::Panel(body);
    auto *form = new QFormLayout(m_form);
    m_name = new QLineEdit(m_form); m_name->setObjectName("styleName");
    m_positive = new QPlainTextEdit(m_form); m_positive->setObjectName("positivePrefix");
    m_negative = new QPlainTextEdit(m_form); m_negative->setObjectName("negativePrefix");
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Positive prefix:"), m_positive);
    form->addRow(tr("Negative prefix:"), m_negative);
    columns->addWidget(m_form, 2); layout->addLayout(columns, 1);
    m_status = new QLabel(body); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    m_save = new KiraUi::PrimaryButton(tr("Save"), body); m_save->setObjectName("saveStyles");
    m_cancel = new QPushButton(tr("Cancel"), body); m_cancel->setObjectName("cancelStyles");
    buttons->addWidget(m_save); buttons->addWidget(m_cancel); layout->addLayout(buttons);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { showStyle(); });
    connect(m_name, &QLineEdit::textChanged, this, [this] { editStyle(); });
    connect(m_positive, &QPlainTextEdit::textChanged, this, [this] { editStyle(); });
    connect(m_negative, &QPlainTextEdit::textChanged, this, [this] { editStyle(); });
    connect(m_add, &QPushButton::clicked, this, [this] {
        m_styles.append({QUuid::createUuid().toString(QUuid::WithoutBraces), tr("New style"), {}, {}});
        m_list->addItem(m_styles.last().name);
        m_list->setCurrentRow(m_styles.size() - 1);
        m_name->setFocus();
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row < 0) return;
        m_styles.removeAt(row);
        { const QSignalBlocker blocker(m_list); delete m_list->takeItem(row); }
        showStyle();
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] { reject(); });
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (!m_client->configureModelStyles(m_modelId, m_styles)) {
            m_status->setText(tr("Could not save styles. Check the connection and style fields."));
            return;
        }
        m_saving = true;
        m_status->setText(tr("Saving styles…"));
        updateActions();
    });
    connect(m_client, &ManagerServiceClient::modelStylesConfigured, this,
        [this](const QString &id, const QList<KiraService::PromptStyle> &) {
            if (m_saving && id == m_modelId) { m_saving = false; accept(); }
        });
    connect(m_client, &ManagerServiceClient::modelOperationFailed, this,
        [this](const QString &id, const QString &message) {
            if (m_saving && id == m_modelId) { m_saving = false; m_status->setText(message); updateActions(); }
        });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this,
        [this](const QString &state, const QString &message) {
            if (!m_client->isAuthenticated() || state == "operation-error") {
                m_saving = false; m_status->setText(message);
            }
            updateActions();
        });
    m_list->setCurrentRow(styles.isEmpty() ? -1 : 0);
    showStyle();
}

void ModelStylesDialog::showStyle()
{
    const QSignalBlocker name(m_name), positive(m_positive), negative(m_negative);
    const int row = m_list->currentRow();
    const auto style = row >= 0 && row < m_styles.size() ? m_styles.at(row) : KiraService::PromptStyle{};
    m_name->setText(style.name);
    m_positive->setPlainText(style.positivePrefix);
    m_negative->setPlainText(style.negativePrefix);
    updateActions();
}

void ModelStylesDialog::editStyle()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_styles.size()) return;
    auto &style = m_styles[row];
    style.name = m_name->text();
    style.positivePrefix = m_positive->toPlainText();
    style.negativePrefix = m_negative->toPlainText();
    m_list->item(row)->setText(style.name);
    updateActions();
}

void ModelStylesDialog::updateActions()
{
    m_list->setEnabled(!m_saving);
    m_form->setEnabled(!m_saving && m_list->currentRow() >= 0);
    m_add->setEnabled(!m_saving);
    m_remove->setEnabled(!m_saving && m_list->currentRow() >= 0);
    m_save->setEnabled(!m_saving && m_client->isAuthenticated() && KiraService::Protocol::validatePromptStyles(m_styles));
    m_cancel->setEnabled(!m_saving);
}

void ModelStylesDialog::reject()
{
    if (!m_saving) KiraUi::Dialog::reject();
}
