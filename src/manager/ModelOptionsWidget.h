#pragma once
#include <QJsonObject>
#include <QMap>
#include <QWidget>
#include "ManagerServiceClient.h"
class QFormLayout;
class QCheckBox;

class ModelOptionsWidget final : public QWidget
{
    Q_OBJECT
  public:
    explicit ModelOptionsWidget(QWidget *parent = nullptr);
    void setOptions(const QString &category, const QJsonObject &options = {}, bool explicitContent = false);
    QJsonObject options() const;
    bool isValid() const;
    bool explicitContent() const;
    void setModels(const QList<ManagedModelInfo>& models);
    // Adds a "New from URL or file" choice to the shared companion selectors of an
    // animation model; options() then holds newCompanionId() for that key.
    void setOffersNewCompanions(bool offers);
    static QString newCompanionId() { return QStringLiteral("new:"); }
    // Selects a shared model by ID in the selector of key, kept across catalogue refreshes.
    void selectModel(const QString &key, const QString &id);
  signals:
    void optionsChanged();

  private:
    bool m_offersNewCompanions = false;
    QCheckBox* m_explicitContent = nullptr;
    QString m_category;
    QFormLayout *m_form;
    QMap<QString, QWidget *> m_fields;
    QList<ManagedModelInfo> m_models;
};
