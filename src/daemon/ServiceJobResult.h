#pragma once

#include <QString>
#include <QStringList>

struct SdCliGenerationResult
{
    bool succeeded = false;
    int exitCode = -1;
    QStringList arguments;
    QString standardOutput;
    QString standardError;
    QString outputPath;
};

Q_DECLARE_METATYPE(SdCliGenerationResult)

struct SdCliUpscaleResult
{
    bool succeeded = false;
    int exitCode = -1;
    QString outputPath;
};

Q_DECLARE_METATYPE(SdCliUpscaleResult)
