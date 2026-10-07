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
  signals:
    void optionsChanged();

  private:
    QCheckBox* m_explicitContent = nullptr;
    QString m_category;
    QFormLayout *m_form;
    QMap<QString, QWidget *> m_fields;
    QList<ManagedModelInfo> m_models;
};
