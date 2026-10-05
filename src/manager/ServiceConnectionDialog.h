#pragma once
#include <KiraUi/Dialog.h>

class ManagerServiceClient;
class QLineEdit;
class QLabel;
class QPushButton;

class ServiceConnectionDialog final : public KiraUi::Dialog
{
    Q_OBJECT
public:
    explicit ServiceConnectionDialog(ManagerServiceClient *client, QWidget *parent = nullptr);
protected:
    void reject() override;
private:
    ManagerServiceClient *m_client;
    QString m_originalHost;
    QString m_originalToken;
    QLineEdit *m_host;
    QLineEdit *m_token;
    QLabel *m_status;
    QPushButton *m_connect;
    bool m_connecting = false;
};
