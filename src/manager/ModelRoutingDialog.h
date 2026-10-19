#pragma once
#include <Ardoise/Dialog.h>
#include <nukleus/ModelRouting.h>

class ManagerServiceClient;
class QStandardItem;
class QStandardItemModel;
class QPlainTextEdit;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QAbstractButton;
namespace Ardoise
{
class TreeView;
}

class ModelRoutingDialog final : public Ardoise::Dialog
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
    QStandardItem *addPresetItem(const Nukleus::GenerationPreset &preset);
    QStandardItem *selectedItem() const;
    Nukleus::ModelRouting configuration() const;
    ManagerServiceClient *m_client;
    Ardoise::TreeView *m_tree;
    QStandardItemModel *m_model;
    QStandardItem *m_generation;
    QStandardItem *m_editing;
    QStackedWidget *m_editor;
    QAbstractButton *m_addPreset;
    QAbstractButton *m_removePreset;
    QWidget *m_prefixes;
    QWidget *m_presetFields;
    QLineEdit *m_presetName;
    QLineEdit *m_presetNameFr;
    QPlainTextEdit *m_positive;
    QPlainTextEdit *m_negative;
    QComboBox *m_identityViews;
    QCheckBox *m_defaultQuality;
    QComboBox *m_assignment;
    QSpinBox *m_steps;
    QDoubleSpinBox *m_cfg;
    QLabel *m_status;
    QPushButton *m_save;
    QPushButton *m_cancel;
    bool m_saving = false;
    bool m_showing = false;
};
