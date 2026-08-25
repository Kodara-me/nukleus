#include "LlamaCliRunner.h"

#include <QDebug>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

namespace
{
constexpr int PromptConversionTimeoutMs = 60'000;

QString promptConversionText(QString output)
{
    const int thinkingEnd = output.lastIndexOf(QStringLiteral("[End thinking]"));
    if (thinkingEnd >= 0) {
        output = output.mid(thinkingEnd + QStringLiteral("[End thinking]").size());
    } else {
        QRegularExpressionMatch lastPrompt;
        const QRegularExpression promptLine(QStringLiteral("(?m)^>.*(?:\\r?\\n|$)"));
        auto match = promptLine.globalMatch(output);
        while (match.hasNext()) lastPrompt = match.next();
        if (lastPrompt.hasMatch()) output = output.mid(lastPrompt.capturedEnd());
    }

    const int timings = output.indexOf(QStringLiteral("\n[ Prompt:"));
    if (timings >= 0) output.truncate(timings);
    const int exit = output.indexOf(QStringLiteral("\nExiting..."));
    if (exit >= 0) output.truncate(exit);
    return output.trimmed();
}
}

LlamaCliRunner::LlamaCliRunner(const QString &executablePath, QObject *parent)
    : QObject(parent)
    , m_executablePath(executablePath)
    , m_process(new QProcess(this))
    , m_timeout(new QTimer(this))
{
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_timeout->setSingleShot(true);
    connect(m_process, &QProcess::started, this, [this] {
        qInfo().noquote() << QStringLiteral("[Kira Service][llama-cli] Starting prompt conversion with Vulkan0.");
        m_timeout->start(PromptConversionTimeoutMs);
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_standardOutput.append(QString::fromLocal8Bit(m_process->readAllStandardOutput()));
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString output = QString::fromLocal8Bit(m_process->readAllStandardError());
        m_standardError.append(output);
        if (!output.trimmed().isEmpty()) qInfo().noquote() << QStringLiteral("[llama-cli][stderr] %1").arg(output.trimmed());
    });
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_running) return;
        m_standardOutput.append(QString::fromLocal8Bit(m_process->readAllStandardOutput()));
        m_standardError.append(QString::fromLocal8Bit(m_process->readAllStandardError()));
        const bool succeeded = !m_timedOut && exitStatus == QProcess::NormalExit && exitCode == 0 && !promptConversionText(m_standardOutput).isEmpty();
        finish(succeeded, m_timedOut ? tr("Prompt conversion timed out after 60 seconds while using Vulkan.") : m_standardError.trimmed());
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_running || error != QProcess::FailedToStart) return;
        finish(false, m_process->errorString());
    });
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (!m_running || m_process->state() == QProcess::NotRunning) return;
        m_timedOut = true;
        qWarning().noquote() << QStringLiteral("[Kira Service][llama-cli] Prompt conversion timed out after 60 seconds; terminating the Vulkan process.");
        cancel();
    });
}

bool LlamaCliRunner::runPromptConversion(const QString &modelPath, const QString &message)
{
    if (m_running || m_executablePath.isEmpty() || modelPath.isEmpty() || message.trimmed().isEmpty()) return false;
    m_running = true;
    m_timedOut = false;
    m_standardOutput.clear();
    m_standardError.clear();
    m_elapsed.start();
    m_process->start(m_executablePath,
                     {QStringLiteral("--model"), modelPath,
                      QStringLiteral("--prompt"), message,
                      QStringLiteral("--ctx-size"), QStringLiteral("2048"),
                      QStringLiteral("--device"), QStringLiteral("Vulkan0"),
                      QStringLiteral("--gpu-layers"), QStringLiteral("all"),
                      QStringLiteral("--n-predict"), QStringLiteral("256"),
                      QStringLiteral("--temp"), QStringLiteral("0.05"),
                      QStringLiteral("--repeat-penalty"), QStringLiteral("1.3"),
                      QStringLiteral("--no-display-prompt"),
                      QStringLiteral("--no-show-timings"),
                      QStringLiteral("--no-conversation"),
                      QStringLiteral("--reasoning"), QStringLiteral("off"),
                      QStringLiteral("--single-turn"),
                      QStringLiteral("--simple-io")});
    return true;
}

void LlamaCliRunner::finish(bool succeeded, const QString &errorMessage)
{
    if (!m_running) return;
    m_running = false;
    m_timeout->stop();
    qInfo().noquote() << QStringLiteral("[Kira Service][llama-cli] Prompt conversion %1 in %2 ms.")
                            .arg(succeeded ? QStringLiteral("finished") : QStringLiteral("failed"))
                            .arg(m_elapsed.elapsed());
    emit promptConversionFinished(succeeded, promptConversionText(m_standardOutput), succeeded ? QString() : errorMessage);
}

void LlamaCliRunner::cancel()
{
    if (!m_running || m_process->state() == QProcess::NotRunning) return;
    m_process->terminate();
    QTimer::singleShot(3000, m_process, [process = m_process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
}
