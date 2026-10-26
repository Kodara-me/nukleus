#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace NukleusWeb
{
struct MultipartFile
{
    QString name;     // the form field
    QString fileName;
    QByteArray data;
};

struct MultipartForm
{
    QHash<QString, QString> fields;
    QList<MultipartFile> files;
};

// Parses a `multipart/form-data` body. Returns false when the content type has no boundary or the
// body is malformed.
bool parseMultipart(const QByteArray &contentType, const QByteArray &body, MultipartForm *form);
}
