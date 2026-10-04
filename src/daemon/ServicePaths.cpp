#include "ServicePaths.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QStandardPaths>

#include <array>

QString ServicePaths::dataRoot()
{
    const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return dataPath.isEmpty() ? QString() : QDir(dataPath).filePath(QStringLiteral("kira-service"));
}

QString ServicePaths::modelsPath()
{
    const QString root = dataRoot();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("models"));
}

QString ServicePaths::upscaleModelsPath()
{
    const QString root = modelsPath();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("upscale"));
}

QString ServicePaths::chatModelsPath()
{
    const QString root = modelsPath();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("llm"));
}

QString ServicePaths::ipAdapterModelsPath()
{
    const QString root = modelsPath();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("ip-adapter"));
}

QString ServicePaths::clipVisionModelsPath()
{
    const QString root = modelsPath();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("clip_vision"));
}

QString ServicePaths::legacyModelsPath()
{
    const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return dataPath.isEmpty() ? QString() : QDir(dataPath).filePath(QStringLiteral("kira-canvas/models"));
}

QString ServicePaths::photoMakerModelsPath()
{
    const QString root = modelsPath();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("photomaker"));
}

bool ServicePaths::migrateLegacyModels(QString *errorMessage)
{
    const QString legacy = legacyModelsPath();
    const QString destination = modelsPath();
    if (legacy.isEmpty() || destination.isEmpty()) {
        if (errorMessage) *errorMessage = QObject::tr("Service data directory is unavailable");
        return false;
    }
    if (!QFileInfo::exists(legacy)) return true;
    if (QFileInfo::exists(destination)) {
        if (errorMessage) *errorMessage = QObject::tr("Both legacy and service model directories exist; move them manually before starting Kira Service");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destination).dir().absolutePath()) || !QDir().rename(legacy, destination)) {
        if (errorMessage) *errorMessage = QObject::tr("Could not migrate legacy model storage");
        return false;
    }
    return true;
}

bool ServicePaths::ensureModelDirectories(QString *errorMessage)
{
    if (!migrateLegacyModels(errorMessage)) return false;
    const QString root = dataRoot();
    if (root.isEmpty()) {
        if (errorMessage) *errorMessage = QObject::tr("Service data directory is unavailable");
        return false;
    }
    const std::array<QString, 7> paths = {root, modelsPath(), upscaleModelsPath(), chatModelsPath(), ipAdapterModelsPath(), clipVisionModelsPath(), photoMakerModelsPath()};
    for (const QString &path : paths) {
        if (!QDir().mkpath(path)) {
            if (errorMessage) *errorMessage = QObject::tr("Could not create %1").arg(path);
            return false;
        }
    }
    return true;
}
