#include "Multipart.h"

#include <QRegularExpression>

namespace NukleusWeb
{
namespace
{
QByteArray boundaryOf(const QByteArray &contentType)
{
    const QString value = QString::fromLatin1(contentType);
    if (!value.trimmed().toLower().startsWith(QStringLiteral("multipart/form-data"))) return {};
    static const QRegularExpression pattern(QStringLiteral("boundary=(?:\"([^\"]+)\"|([^;\\s]+))"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch()) return {};
    return (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1)).toLatin1();
}

QString parameter(const QString &header, const QString &name)
{
    const QRegularExpression pattern(QStringLiteral("(?:^|;)\\s*%1=\"([^\"]*)\"").arg(name), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(header);
    return match.hasMatch() ? match.captured(1) : QString();
}
}

bool parseMultipart(const QByteArray &contentType, const QByteArray &body, MultipartForm *form)
{
    const QByteArray boundary = boundaryOf(contentType);
    if (boundary.isEmpty() || boundary.size() > 200) return false;
    const QByteArray delimiter = "--" + boundary;
    qsizetype position = body.indexOf(delimiter);
    if (position != 0) return false;
    *form = {};
    bool closed = false;
    while (position >= 0 && !closed) {
        qsizetype start = position + delimiter.size();
        if (body.mid(start, 2) == "--") { closed = true; break; }
        if (body.mid(start, 2) != "\r\n") return false;
        start += 2;
        const qsizetype next = body.indexOf("\r\n" + delimiter, start);
        if (next < 0) return false;
        const QByteArray part = body.mid(start, next - start);
        const qsizetype headerEnd = part.indexOf("\r\n\r\n");
        if (headerEnd < 0) return false;
        QString disposition;
        for (const QByteArray &line : part.left(headerEnd).split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith("content-disposition:")) disposition = QString::fromUtf8(trimmed.mid(20)).trimmed();
        }
        const QString name = parameter(disposition, QStringLiteral("name"));
        if (name.isEmpty()) return false;
        const QByteArray data = part.mid(headerEnd + 4);
        if (disposition.contains(QStringLiteral("filename="), Qt::CaseInsensitive))
            form->files.append({name, parameter(disposition, QStringLiteral("filename")), data});
        else
            form->fields.insert(name, QString::fromUtf8(data));
        position = next + 2;
    }
    return closed;
}
}
