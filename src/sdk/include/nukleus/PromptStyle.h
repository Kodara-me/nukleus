#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QMetaType>
#include <QString>
#include <optional>

namespace Nukleus
{
struct PromptStyle
{
    QString id;
    QString name;
    QString positivePrefix;
    QString negativePrefix;
    bool operator==(const PromptStyle &) const = default;
};

namespace Protocol
{
bool validatePromptStyles(const QList<PromptStyle> &styles, QString *error = nullptr);
std::optional<QList<PromptStyle>> promptStylesFromJson(const QJsonValue &value, QString *error = nullptr);
QJsonArray promptStylesToJson(const QList<PromptStyle> &styles);
QList<PromptStyle> starterPromptStyles();
QString composeStyledPrompt(const QString &prefix, const QString &prompt);
}
}

Q_DECLARE_METATYPE(Nukleus::PromptStyle)
