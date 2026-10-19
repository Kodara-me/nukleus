#include "ServiceConnectionDialog.h"
#include "ManagerServiceClient.h"
#include <Ardoise/PrimaryButton.h>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

ServiceConnectionDialog::ServiceConnectionDialog(ManagerServiceClient *client, QWidget *parent)
    : Ardoise::Dialog(parent), m_client(client), m_originalHost(client->host()), m_originalToken(client->token())
{
    setWindowTitle(tr("Connect to Nukleus"));
    setModal(true);
    auto *body = new QWidget(this);
    setContentWidget(body);
    auto *form = new QFormLayout(body);
    m_host = new QLineEdit(m_originalHost, body);
    m_host->setObjectName("serviceHostInput");
    m_host->setPlaceholderText("127.0.0.1");
    form->addRow(tr("Service IP:"), m_host);
    m_token = new QLineEdit(m_originalToken, body);
    m_token->setObjectName("serviceTokenInput");
    m_token->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Key:"), m_token);
    m_status = new QLabel(tr("Enter an IPv4 address or localhost, without ws:// or a port."), body);
    m_status->setObjectName("connectionStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    form->addRow(m_status);
    auto *buttons = new QHBoxLayout;
    m_connect = new Ardoise::PrimaryButton(tr("Connect"), body);
    m_connect->setObjectName("connectService");
    m_connect->setDefault(true);
    auto *cancel = new QPushButton(tr("Cancel"), body);
    cancel->setObjectName("cancelConnection");
    buttons->addStretch(); buttons->addWidget(m_connect); buttons->addWidget(cancel);
    form->addRow(buttons);
    connect(cancel, &QPushButton::clicked, this, &ServiceConnectionDialog::reject);
    connect(m_connect, &QPushButton::clicked, this, [this] {
        if (!m_client->setConnection(m_host->text(), m_token->text())) {
            m_status->setText(tr("Enter an IPv4 address or localhost, without ws:// or a port."));
            return;
        }
        m_connecting = true;
        m_connect->setEnabled(false);
        m_host->setEnabled(false); m_token->setEnabled(false);
        m_client->connectToService();
    });
    connect(m_client, &ManagerServiceClient::serviceStateChanged, this,
        [this](const QString &state, const QString &message) {
            if (!m_connecting) return;
            m_status->setText(message);
            if (state == "connected" && m_client->isAuthenticated()) {
                QSettings settings;
                settings.setValue("Service/Host", m_client->host());
                settings.setValue("Service/Token", m_client->token());
                m_connecting = false;
                accept();
            } else if (state == "connection-error" || state == "authentication-failed" || state == "offline") {
                m_connecting = false;
                m_connect->setEnabled(true);
                m_host->setEnabled(true); m_token->setEnabled(true);
            }
        });
    resize(440, sizeHint().height());
}

void ServiceConnectionDialog::reject()
{
    m_connecting = false;
    m_client->setConnection(m_originalHost, m_originalToken);
    Ardoise::Dialog::reject();
}
