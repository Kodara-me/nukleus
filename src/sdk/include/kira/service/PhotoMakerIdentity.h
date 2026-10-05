#pragma once

#include <QByteArray>
#include <QString>
#include <QList>

namespace KiraService {
struct PhotoMakerIdentityPackage {
    QList<QByteArray> references;
    QByteArray embeddings;
    QString identityPrompt;
    QString hairPrompt;
    int seed = -1;
};
QByteArray packPhotoMakerIdentity(const PhotoMakerIdentityPackage& identity, QString* error = nullptr);
bool unpackPhotoMakerIdentity(const QByteArray& package, PhotoMakerIdentityPackage& identity, QString* error = nullptr);
// Version 1 .kiraid ZIP: manifest.json, one to four PNG references, id_embeds.bin.
QByteArray packPhotoMakerIdentity(const QByteArray &referencePng, const QByteArray &embeddings,
    QString *error = nullptr);
bool unpackPhotoMakerIdentity(const QByteArray &package, QByteArray &referencePng,
    QByteArray &embeddings, QString *error = nullptr);
}
