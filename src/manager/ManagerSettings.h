#pragma once

#include <QString>

namespace ManagerSettings
{
// Copies settings saved under the former Kira Service names into the current
// application settings when those are still empty. The legacy file is kept.
bool migrateLegacySettings(const QString &legacyOrganization = QStringLiteral("Kira Service"),
                           const QString &legacyApplication = QStringLiteral("Kira Service Manager"));
}
