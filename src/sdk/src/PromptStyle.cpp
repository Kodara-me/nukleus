#include <kira/service/PromptStyle.h>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace KiraService::Protocol
{
bool validatePromptStyles(const QList<PromptStyle> &styles, QString *error)
{
    QSet<QString> ids;
    for (const auto &style : styles) {
        if (style.id.trimmed().isEmpty() || style.name.trimmed().isEmpty()
            || (style.positivePrefix.trimmed().isEmpty() && style.negativePrefix.trimmed().isEmpty())
            || ids.contains(style.id)) {
            if (error) *error = QStringLiteral("Styles require unique nonblank IDs, names and at least one prefix");
            return false;
        }
        ids.insert(style.id);
    }
    return true;
}

std::optional<QList<PromptStyle>> promptStylesFromJson(const QJsonValue &value, QString *error)
{
    const auto invalid = [error]() -> std::optional<QList<PromptStyle>> {
        if (error) *error = QStringLiteral("Styles must be an array of objects with string fields");
        return std::nullopt;
    };
    if (!value.isArray()) return invalid();
    QList<PromptStyle> styles;
    for (const auto &item : value.toArray()) {
        if (!item.isObject()) return invalid();
        const auto object = item.toObject();
        for (const auto &key : {"id", "name", "positivePrefix", "negativePrefix"})
            if (!object.value(key).isString()) return invalid();
        styles.append({object.value("id").toString(), object.value("name").toString(),
            object.value("positivePrefix").toString(), object.value("negativePrefix").toString()});
    }
    if (!validatePromptStyles(styles, error)) return std::nullopt;
    return styles;
}

QJsonArray promptStylesToJson(const QList<PromptStyle> &styles)
{
    QJsonArray array;
    for (const auto &style : styles)
        array.append(QJsonObject{{"id", style.id}, {"name", style.name},
            {"positivePrefix", style.positivePrefix}, {"negativePrefix", style.negativePrefix}});
    return array;
}

QList<PromptStyle> starterPromptStyles()
{
    return {{"photographic", "Photographic", "photorealistic, natural lighting, fine detail", "illustration, painting, cartoon"},
        {"cinematic", "Cinematic", "cinematic lighting, dramatic composition, film still", ""},
        {"illustration", "Illustration", "digital illustration, expressive colors, detailed artwork", "photograph, photorealistic"}};
}

QString composeStyledPrompt(const QString &prefix, const QString &prompt)
{
    QStringList parts;
    if (!prefix.trimmed().isEmpty()) parts.append(prefix.trimmed());
    if (!prompt.trimmed().isEmpty()) parts.append(prompt.trimmed());
    return parts.join(QStringLiteral(", "));
}
}
