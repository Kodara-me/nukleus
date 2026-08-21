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
    void updateDeleteButton();
    void startInstall();
    void deleteSelectedModel();

    ManagerServiceClient *m_client;
    QLabel *m_status = nullptr;
    QListWidget *m_generationModels = nullptr;
    QListWidget *m_upscaleModels = nullptr;
    QListWidget *m_llmChatModels = nullptr;
    QListWidget *m_ipAdapterModels = nullptr;
    QComboBox *m_category = nullptr;
    QLineEdit *m_url = nullptr;
    QLabel *m_companionLabel = nullptr;
    QLineEdit *m_companionUrl = nullptr;
    QLineEdit *m_sha256 = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QTreeWidget *m_installs = nullptr;
};
