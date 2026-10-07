#pragma once
#include <KiraUi/Dialog.h>
#include <kira/service/ModelRouting.h>

class ManagerServiceClient;
class QStandardItem;
class QStandardItemModel;
class QPlainTextEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QSpinBox;
class QDoubleSpinBox;
namespace KiraUi
{
class TreeView;
}

class ModelRoutingDialog final : public KiraUi::Dialog
{
    Q_OBJECT
public:
    explicit ModelRoutingDialog(ManagerServiceClient *client, QWidget *parent = nullptr);

protected:
    void reject() override;

private:
    void showSelection();
    void updateActions();
    void refreshModels();
    QStandardItem *selectedItem() const;
    KiraService::ModelRouting configuration() const;
    ManagerServiceClient *m_client;
    KiraUi::TreeView *m_tree;
    QStandardItemModel *m_model;
    QStandardItem *m_generation;
    QStandardItem *m_editing;
    QStackedWidget *m_editor;
    QPlainTextEdit *m_positive;
    QPlainTextEdit *m_negative;
    QComboBox *m_assignment;
    QSpinBox *m_steps;
    QDoubleSpinBox *m_cfg;
    QLabel *m_status;
    QPushButton *m_save;
    QPushButton *m_cancel;
    bool m_saving = false;
    bool m_showing = false;
};
