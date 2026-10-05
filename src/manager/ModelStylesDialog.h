#pragma once

#include <KiraUi/Dialog.h>
#include <kira/service/PromptStyle.h>

class ManagerServiceClient;
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;

class ModelStylesDialog final : public KiraUi::Dialog
{
    Q_OBJECT
public:
    ModelStylesDialog(ManagerServiceClient *client, const QString &modelId,
                      const QList<KiraService::PromptStyle> &styles, QWidget *parent = nullptr);
protected:
    void reject() override;
private:
    void showStyle();
    void editStyle();
    void updateActions();
    ManagerServiceClient *m_client;
    QString m_modelId;
    QList<KiraService::PromptStyle> m_styles;
    QListWidget *m_list;
    QWidget *m_form;
    QLineEdit *m_name;
    QPlainTextEdit *m_positive;
    QPlainTextEdit *m_negative;
    QLabel *m_status;
    QPushButton *m_add;
    QPushButton *m_remove;
    QPushButton *m_save;
    QPushButton *m_cancel;
    bool m_saving = false;
};
