#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

class LlamaCliRunner final : public QObject
{
    Q_OBJECT

public:
    explicit LlamaCliRunner(const QString &executablePath, QObject *parent = nullptr);

    bool runPromptConversion(const QString &modelPath, const QString &message);
    void cancel();

signals:
    void promptConversionFinished(bool succeeded, const QString &text, const QString &errorMessage);

private:
    void finish(bool succeeded, const QString &errorMessage = {});

    QString m_executablePath;
    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;
    QString m_standardOutput;
    QString m_standardError;
    QElapsedTimer m_elapsed;
    bool m_running = false;
    bool m_timedOut = false;
};
