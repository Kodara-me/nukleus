#include "ManagerSettings.h"

#include <QSettings>

bool ManagerSettings::migrateLegacySettings(const QString &legacyOrganization, const QString &legacyApplication)
{
    QSettings current;
    if (!current.allKeys().isEmpty()) return false;
    const QSettings legacy(QSettings::defaultFormat(), QSettings::UserScope, legacyOrganization, legacyApplication);
    const QStringList keys = legacy.allKeys();
    if (keys.isEmpty()) return false;
    for (const QString &key : keys) current.setValue(key, legacy.value(key));
    current.sync();
    return current.status() == QSettings::NoError;
}
