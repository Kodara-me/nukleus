#pragma once
#include <QJsonObject>
#include <QMap>
#include <QWidget>
class QFormLayout;
class QCheckBox;

class ModelOptionsWidget final : public QWidget
{
    Q_OBJECT
  public:
    explicit ModelOptionsWidget(QWidget *parent = nullptr);
    void setOptions(const QString &category, const QJsonObject &options = {}, bool explicitContent = false, bool canEdit = false);
    QJsonObject options() const;
    bool isValid() const;
    bool explicitContent() const;
    bool canEdit() const;
  signals:
    void optionsChanged();

  private:
    QCheckBox* m_explicitContent = nullptr;
    QCheckBox* m_canEdit = nullptr;
    QString m_category;
    QFormLayout *m_form;
    QMap<QString, QWidget *> m_fields;
};
