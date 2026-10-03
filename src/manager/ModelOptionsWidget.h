#pragma once
#include <QJsonObject>
#include <QMap>
#include <QWidget>
class QFormLayout;

class ModelOptionsWidget final : public QWidget
{
    Q_OBJECT
  public:
    explicit ModelOptionsWidget(QWidget *parent = nullptr);
    void setOptions(const QString &category, const QJsonObject &options = {});
    QJsonObject options() const;
    bool isValid() const;
  signals:
    void optionsChanged();

  private:
    QString m_category;
    QFormLayout *m_form;
    QMap<QString, QWidget *> m_fields;
};
