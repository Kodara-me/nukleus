#pragma once

#include <QString>

class ServicePaths final
{
public:
    static QString dataRoot();
    static QString modelsPath();
    static QString upscaleModelsPath();
    static QString chatModelsPath();
    static QString ipAdapterModelsPath();
    static QString clipVisionModelsPath();
    static QString photoMakerModelsPath();
    static QString promptEnhancerModelsPath();
    static QString legacyModelsPath();
    static bool migrateLegacyModels(QString *errorMessage = nullptr);
    static bool ensureModelDirectories(QString *errorMessage = nullptr);
};
