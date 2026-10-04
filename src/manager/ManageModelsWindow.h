#pragma once
#include <KiraUi/MainWindow.h>
#include <QJsonObject>
#include <functional>
class ManagerServiceClient;
class ModelOptionsWidget;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QSplitter;
class QCloseEvent;
namespace KiraUi
{
class TreeView;
}

class ManageModelsWindow final : public KiraUi::MainWindow
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
    void deleteSelectedModel();
    void saveOptions();
    bool guardChanges(std::function<void()> continuation);
    ManagerServiceClient *m_client;
    QLabel *m_status;
    QLabel *m_details;
    KiraUi::TreeView *m_tree;
    QStandardItemModel *m_treeModel;
    QSplitter *m_splitter;
    ModelOptionsWidget *m_options;
    QPushButton *m_add;
    QPushButton *m_attach;
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
