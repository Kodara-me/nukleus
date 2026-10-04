#pragma once

#include <QByteArray>
#include <QString>

namespace KiraService {
// Version 1 .kiraid ZIP: manifest.json, reference.png, id_embeds.bin.
QByteArray packPhotoMakerIdentity(const QByteArray &referencePng, const QByteArray &embeddings,
    QString *error = nullptr);
bool unpackPhotoMakerIdentity(const QByteArray &package, QByteArray &referencePng,
    QByteArray &embeddings, QString *error = nullptr);
}
