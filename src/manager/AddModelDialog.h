#pragma once
#include <Ardoise/Dialog.h>
#include <QJsonObject>
class ManagerServiceClient;
class ModelOptionsWidget;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QProgressBar;
class QCloseEvent;

class AddModelDialog final : public Ardoise::Dialog
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
    QString roleFor(QLineEdit* field) const;
    QString m_pendingRemoval;
    void updateCategory();
    void updateActions();
    void startInstall();
    void updateProgress();
    ManagerServiceClient *m_client;
    QComboBox *m_category;
    QLineEdit *m_url;
    QLineEdit *m_displayName;
    QComboBox* m_mmprojSelection;
    QLabel* m_mmprojSelectionLabel;
    QWidget* m_recommendations;
    void refreshCompanions();
    QLineEdit *m_companion;
    QComboBox *m_modelType;
    QLabel *m_modelTypeLabel;
    QString m_existingModelId;
    QJsonObject m_attachedFiles;
    QLabel *m_companionLabel;
    ModelOptionsWidget *m_options = nullptr;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_start;
    QPushButton *m_cancel;
    QString m_installId;
    bool m_busy = false;
    bool m_canceling = false;
};
