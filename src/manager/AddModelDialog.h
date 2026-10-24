#pragma once
#include <Ardoise/Dialog.h>
#include <QJsonObject>
#include <QStringList>
class ManagerServiceClient;
class ModelOptionsWidget;
class QComboBox;
class QFormLayout;
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
    void startNextStep();
    bool startModelInstall();
    bool startCompanionInstall(const QString &key);
    void updateProgress();
    void showRow(QLineEdit *field, bool visible);
    QString fileRoleLabel(const QString &role) const;
    QList<QLineEdit*> animationFiles() const;
    bool isAnimation() const;
    ManagerServiceClient *m_client;
    QFormLayout *m_form;
    QComboBox *m_category;
    QLineEdit *m_url;
    QLabel *m_urlLabel;
    QLineEdit *m_displayName;
    QComboBox* m_mmprojSelection;
    QLabel* m_mmprojSelectionLabel;
    QWidget* m_recommendations;
    void refreshCompanions();
    QLineEdit *m_companion;
    QLineEdit *m_highNoise;
    QLineEdit *m_loraLowNoise;
    QLineEdit *m_loraHighNoise;
    QLineEdit *m_newVae;
    QLineEdit *m_newTextEncoder;
    QComboBox *m_modelType;
    QLabel *m_modelTypeLabel;
    QString m_existingModelId;
    QJsonObject m_attachedFiles;
    QStringList m_missingFiles;
    QLabel *m_companionLabel;
    ModelOptionsWidget *m_options = nullptr;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_start;
    QPushButton *m_cancel;
    QString m_installId;
    // Shared companions still to install before an animation model, as recommendedOptions keys.
    QStringList m_pendingCompanions;
    QString m_installingCompanion;
    int m_step = 0;
    int m_stepCount = 1;
    bool m_busy = false;
    bool m_canceling = false;
};
