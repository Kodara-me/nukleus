#pragma once

#include <QString>
#include <QStringList>

struct GenerationRequest
{
    QString prompt;
    QString negativePrompt;
    QString sampler;
    QString scheduler;
    int width = 1024;
    int height = 1024;
    int steps = 28;
    double cfgScale = 7.0;
    int clipSkip = -1;
    int seed = -1;
    QStringList referenceImagePaths;
    QString baseImagePath;
    double baseImageStrength = 0.55;
    QString outputPath;
    QString modelPath;
    QString vaePath;
    QString llmPath;
    QString llmVisionPath;
    QString clipLPath;
    QString clipGPath;
    bool diffusionModel = false;
};
