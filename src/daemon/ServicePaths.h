#pragma once

#include <QString>

class ServicePaths final
{
public:
    static QString dataRoot();
    // Each lora.train job's folder (images, captions, dataset configuration, trainer output) lives here, on disk
    // and not under TMPDIR, which is a RAM-backed directory on Linux.
    static QString trainingPath();
    // Caches the LoRA trainer's libraries write (ROCm kernels, compiler output, ...), kept between jobs.
    static QString trainerCachePath();
    // Removes everything under trainingPath(). Returns false, naming the entry, when one cannot be removed.
    static bool emptyTrainingPath(QString *errorMessage = nullptr);
    static QString modelsPath();
    static QString upscaleModelsPath();
    static QString chatModelsPath();
    static QString legacyDataRoot();
    static bool migrateLegacyDataRoot(QString *errorMessage = nullptr);
    static QString legacyModelsPath();
    static bool migrateLegacyModels(QString *errorMessage = nullptr);
    static bool ensureModelDirectories(QString *errorMessage = nullptr);
};
