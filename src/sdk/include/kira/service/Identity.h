#pragma once
#include <QByteArray>
#include <QString>
#include <QList>

namespace KiraService {
struct IdentityPackage {
    QList<QByteArray> references;
    QString identityPrompt;
    QString hairPrompt;
    int seed = -1;
};
// Version 2 .kiraid ZIP: manifest.json and one to four PNG references.
QByteArray packIdentity(const IdentityPackage& identity, QString* error = nullptr);
bool unpackIdentity(const QByteArray& package, IdentityPackage& identity, QString* error = nullptr);
}
