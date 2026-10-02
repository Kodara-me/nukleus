#pragma once

#include <QMainWindow>

class ManagerServiceClient;
class QComboBox;
class QLineEdit;
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
    QTreeWidget *m_models = nullptr;
    QComboBox *m_category = nullptr;
    QLineEdit *m_url = nullptr;
    QLabel *m_companionLabel = nullptr;
    QLineEdit *m_companionUrl = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QTreeWidget *m_installs = nullptr;
};
