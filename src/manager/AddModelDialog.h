#pragma once
#include <KiraUi/Dialog.h>
#include <QJsonObject>
class ManagerServiceClient;
class ModelOptionsWidget;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QProgressBar;
class QCloseEvent;

class AddModelDialog final : public KiraUi::Dialog
{
    Q_OBJECT
  public:
    explicit AddModelDialog(ManagerServiceClient *client, const QString &category = {},
                            QWidget *parent = nullptr, const QString &existingModelId = {});
  signals:
    void modelInstalled(const QString &id);

  protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;

  private:
    void updateCategory();
    void updateActions();
    void startInstall();
    void updateProgress();
    ManagerServiceClient *m_client;
    QComboBox *m_category;
    QLineEdit *m_url;
    QLineEdit *m_companion;
    QLineEdit *m_clipL;
    QLineEdit *m_clipG;
    QLabel *m_clipLLabel;
    QLabel *m_clipGLabel;
    QComboBox *m_modelType;
    QLabel *m_modelTypeLabel;
    QString m_existingModelId;
    QJsonObject m_attachedFiles;
    QLabel *m_companionLabel;
    ModelOptionsWidget *m_options;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_start;
    QPushButton *m_cancel;
    QString m_installId;
    bool m_busy = false;
    bool m_canceling = false;
};
