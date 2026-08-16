#pragma once

#include <QMainWindow>

class ManagerServiceClient;
class QComboBox;
class QLineEdit;
class QListWidget;
class QLabel;
class QPushButton;
class QTreeWidget;

class ManageModelsWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit ManageModelsWindow(QWidget *parent = nullptr);

private:
    void populateModels();
    void populateInstalls();
    void updateInstallButton();
    void connectService();
    void startInstall();
    bool validLocalHost(const QString &host) const;

    ManagerServiceClient *m_client;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_token = nullptr;
    QLabel *m_status = nullptr;
    QListWidget *m_generationModels = nullptr;
    QListWidget *m_upscaleModels = nullptr;
    QListWidget *m_llmChatModels = nullptr;
    QComboBox *m_category = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_sha256 = nullptr;
    QPushButton *m_installButton = nullptr;
    QTreeWidget *m_installs = nullptr;
};
