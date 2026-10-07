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
class QLineEdit;
class QListWidget;
class QAbstractButton;
class QJsonArray;
class QJsonObject;
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
    void showFlow(int row);
    void editStep(const QString &key, const QString &value);
    void setFlow(QStandardItem *item, const QJsonArray &flow);
    QString stepLabel(int row, const QJsonObject &step) const;
    QStandardItem *selectedItem() const;
    KiraService::ModelRouting configuration() const;
    ManagerServiceClient *m_client;
    KiraUi::TreeView *m_tree;
    QStandardItemModel *m_model;
    QStandardItem *m_generation;
    QStandardItem *m_editing;
    QStackedWidget *m_editor;
    QWidget *m_prefixes;
    QPlainTextEdit *m_positive;
    QPlainTextEdit *m_negative;
    QComboBox *m_identityViews;
    QComboBox *m_assignment;
    QSpinBox *m_steps;
    QDoubleSpinBox *m_cfg;
    QListWidget *m_flowSteps;
    QAbstractButton *m_addStep;
    QAbstractButton *m_moveStepUp;
    QAbstractButton *m_moveStepDown;
    QAbstractButton *m_removeStep;
    QWidget *m_stepEditor;
    QLineEdit *m_stepName;
    QLineEdit *m_stepNameFr;
    QComboBox *m_stepModel;
    QPlainTextEdit *m_stepInstructions;
    QLabel *m_status;
    QPushButton *m_save;
    QPushButton *m_cancel;
    bool m_saving = false;
    bool m_showing = false;
};
