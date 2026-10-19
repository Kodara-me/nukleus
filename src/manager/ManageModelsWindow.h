#pragma once
#include <Ardoise/MainWindow.h>
#include <QJsonObject>
#include <functional>
class ManagerServiceClient;
class ModelOptionsWidget;
class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class QSplitter;
class QCloseEvent;
class ServiceConnectionDialog;
namespace Ardoise
{
class TreeView;
}

class ManageModelsWindow final : public Ardoise::MainWindow
{
    Q_OBJECT
  public:
    explicit ManageModelsWindow(QWidget *parent = nullptr, ManagerServiceClient *client = nullptr);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void populateModels();
    void selectModel(const QString &id);
    void showSelection(const QString &id);
    void selectionChanged();
    void updateActions();
    void addModel();
    void attachFiles();
    void editPresets();
    void deleteSelectedModel();
    void saveOptions();
    bool guardChanges(std::function<void()> continuation);
    void showConnectionDialog();
    ServiceConnectionDialog *m_connectionDialog = nullptr;
    bool m_connectionPrompted = false;
    ManagerServiceClient *m_client;
    QLabel *m_status;
    QLabel *m_details;
    QWidget* m_configurationBody;
    QLabel* m_mmprojLabel;
    QComboBox* m_mmproj;
    QString m_originalMmproj;
    void refreshCompanions();
    QLineEdit *m_displayName;
    QString m_originalDisplayName;
    Ardoise::TreeView *m_tree;
    QStandardItemModel *m_treeModel;
    QSplitter *m_splitter;
    ModelOptionsWidget *m_options;
    QPushButton *m_add;
    QPushButton *m_attach;
    QPushButton *m_presets;
    QPushButton *m_delete;
    QPushButton *m_save;
    QString m_selectedId;
    QJsonObject m_originalOptions;
    bool m_originalExplicitContent = false;
    bool m_dirty = false;
    bool m_saving = false;
    bool m_allowClose = false;
    std::function<void()> m_afterSave;
};
