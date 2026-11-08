#pragma once

#include <QList>
#include <QPair>
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
    QString maskImagePath;
    double baseImageStrength = 0.55;
    QString controlNetPath;
    QString controlImagePath;
    double controlStrength = 0.9;
    QString loraDirectory; // --lora-model-dir, set when loras is not empty.
    QList<QPair<QString, double>> loras; // LoRA file name without extension and weight, in request order.
    QString outputPath;
    QString modelPath;
    QString vaePath;
    QString llmPath;
    QString llmVisionPath;
    QString clipLPath;
    QString clipGPath;
    bool diffusionModel = false;
};
