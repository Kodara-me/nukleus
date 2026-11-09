#include "LoraTrainerRunner.h"

#include "ProcessTree.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>

namespace
{
constexpr int ErrorTailCharacters = 16384;
constexpr int VersionTimeoutMs = 15000;
// Percentages of a training job: the stages before it take 0-4, the steps 5-95, and saving and completion
// the rest.
constexpr int FirstStepPercentage = 5;
constexpr int StepPercentageSpan = 90;

QString quoted(const QString &argument)
{
    QString text = argument;
    text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + text + QLatin1Char('\'');
}

// tqdm's bar of sd-scripts' training loop: "steps:   8%|▊         | 120/1500 [01:23<15:00,  1.5it/s, ...]".
// Other bars (caching latents, ...) carry no "steps:" description.
const QRegularExpression &stepsBar()
{
    static const QRegularExpression expression(QStringLiteral("^\\s*steps:\\s*\\d+%\\|[^|]*\\|\\s*(\\d+)/(\\d+)"));
    return expression;
}
}

LoraTrainerRunner::LoraTrainerRunner(QString trainerDirectory, QObject *parent)
    : QObject(parent)
    , m_directory(std::move(trainerDirectory))
{
    qRegisterMetaType<LoraTrainerResult>();
    qRegisterMetaType<Nukleus::GenerationProgress>();
}

LoraTrainerRunner::~LoraTrainerRunner()
{
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(3000);
        }
    }
    // The tree kills whatever remains of the run when it is destroyed.
    delete m_tree;
    m_tree = nullptr;
}

QString LoraTrainerRunner::directory() const
{
    return m_directory;
}

QString LoraTrainerRunner::scriptPath() const
{
    return QDir(m_directory).filePath(QStringLiteral("sdxl_train_network.py"));
}

QString LoraTrainerRunner::pythonPath() const
{
#ifdef Q_OS_WIN
    return QDir(m_directory).filePath(QStringLiteral("venv/Scripts/python.exe"));
#else
    return QDir(m_directory).filePath(QStringLiteral("venv/bin/python"));
#endif
}

bool LoraTrainerRunner::isReady() const
{
    if (m_directory.isEmpty() || !QFileInfo(scriptPath()).isFile() || !QFileInfo(pythonPath()).isFile()) return false;
    QProcess version;
    version.setProgram(pythonPath());
    version.setArguments({QStringLiteral("--version")});
    version.setStandardInputFile(QProcess::nullDevice());
    version.setStandardOutputFile(QProcess::nullDevice());
    version.setStandardErrorFile(QProcess::nullDevice());
    version.start();
    if (!version.waitForFinished(VersionTimeoutMs)) {
        version.kill();
        version.waitForFinished(3000);
        return false;
    }
    return version.exitStatus() == QProcess::NormalExit && version.exitCode() == 0;
}

QString LoraTrainerRunner::commandLine(const QStringList &arguments) const
{
    QStringList parts{quoted(pythonPath()), quoted(scriptPath())};
    for (const QString &argument : arguments) parts.append(quoted(argument));
    return parts.join(QLatin1Char(' '));
}

bool LoraTrainerRunner::isRunning() const
{
    return m_running;
}

bool LoraTrainerRunner::run(const QStringList &arguments, const QString &workingDirectory,
                            const QProcessEnvironment &environment, int totalSteps)
{
    if (m_running) return false;
    m_arguments = arguments;
    m_totalSteps = std::max(totalSteps, 1);
    m_lastStep = -1;
    m_percentage = 0;
    m_canceled = false;
    m_processDone = false;
    m_treeEnded = false;
    m_reported = false;
    m_attachFailed = false;
    m_exitCode = -1;
    m_normalExit = false;
    m_pendingOutput.clear();
    m_pendingError.clear();
    m_standardError.clear();

    m_process = new QProcess(this);
    m_tree = new ProcessTree(this);
    m_process->setProgram(pythonPath());
    m_process->setArguments(QStringList{scriptPath()} + arguments);
    m_process->setWorkingDirectory(workingDirectory);
    m_process->setProcessEnvironment(environment);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->setStandardInputFile(QProcess::nullDevice());
    m_tree->prepare(*m_process);

    connect(m_process, &QProcess::started, this, [this] {
        QString error;
        if (!m_tree->attach(*m_process, &error)) {
            m_attachFailed = true;
            m_standardError += error + QLatin1Char('\n');
            qWarning().noquote() << QStringLiteral("[Nukleus][lora-trainer] %1").arg(error);
            m_process->kill();
            return;
        }
        if (m_canceled) {
            m_process->kill();
            m_tree->stop();
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] { readOutput(true); });
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        processFinished(exitCode, status == QProcess::NormalExit);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_standardError += m_process->errorString() + QLatin1Char('\n');
        processFinished(-1, false);
    });
    connect(m_tree, &ProcessTree::ended, this, [this] {
        m_treeEnded = true;
        finishIfDone();
    });

    m_running = true;
    qInfo().noquote() << QStringLiteral("[Nukleus][lora-trainer] Starting LoRA training");
    qInfo().noquote() << QStringLiteral("[Nukleus][lora-trainer] Working directory: %1").arg(workingDirectory);
    qInfo().noquote() << QStringLiteral("[Nukleus][lora-trainer] Command:\n%1").arg(commandLine(arguments));
    m_process->start();
    return true;
}

void LoraTrainerRunner::cancel()
{
    if (!m_running || m_canceled) return;
    m_canceled = true;
    if (m_process && m_process->state() == QProcess::Starting) m_process->kill();
    if (m_tree) m_tree->stop();
}

void LoraTrainerRunner::processFinished(int exitCode, bool normalExit)
{
    if (!m_running || m_processDone) return;
    readOutput(false);
    readOutput(true);
    // The last, unterminated line of each stream.
    if (!m_pendingOutput.isEmpty()) processLine(m_pendingOutput, false);
    if (!m_pendingError.isEmpty()) processLine(m_pendingError, true);
    m_pendingOutput.clear();
    m_pendingError.clear();
    m_processDone = true;
    m_exitCode = exitCode;
    m_normalExit = normalExit;
    // The trainer's children are stopped at every end of a run, not only on cancellation: none may still
    // hold the GPU when the next job starts.
    m_tree->stop();
    finishIfDone();
}

void LoraTrainerRunner::finishIfDone()
{
    if (!m_running || m_reported || !m_processDone || !m_treeEnded) return;
    m_reported = true;
    m_running = false;
    LoraTrainerResult result;
    result.canceled = m_canceled;
    result.exitCode = m_exitCode;
    result.succeeded = m_normalExit && m_exitCode == 0 && !m_canceled && !m_attachFailed;
    result.command = commandLine(m_arguments);
    result.standardError = m_standardError;
    qInfo().noquote() << QStringLiteral("[Nukleus][lora-trainer] Finished (exit code %1%2)")
                             .arg(m_exitCode).arg(m_canceled ? QStringLiteral(", canceled") : QString());
    m_process->disconnect(this);
    m_process->deleteLater();
    m_tree->disconnect(this);
    m_tree->deleteLater();
    m_process = nullptr;
    m_tree = nullptr;
    emit finished(result);
}

void LoraTrainerRunner::readOutput(bool standardError)
{
    if (!m_process) return;
    const QByteArray data = standardError ? m_process->readAllStandardError() : m_process->readAllStandardOutput();
    if (data.isEmpty()) return;
    QString &pending = standardError ? m_pendingError : m_pendingOutput;
    pending.append(QString::fromUtf8(data));
    static const QRegularExpression lineEnd(QStringLiteral("[\\r\\n]"));
    for (;;) {
        const qsizetype end = pending.indexOf(lineEnd);
        if (end < 0) break;
        const QString line = pending.left(end);
        pending.remove(0, end + 1);
        if (!line.isEmpty()) processLine(line, standardError);
    }
    // A progress bar is redrawn in place and ends with no terminator until it completes: read it now.
    if (!pending.isEmpty()) {
        const auto match = stepsBar().match(pending);
        if (match.hasMatch()) processLine(pending, standardError);
    }
}

void LoraTrainerRunner::processLine(const QString &line, bool standardError)
{
    const auto match = stepsBar().match(line);
    if (match.hasMatch()) {
        // The percentage never decreases: a step is reported once, and only when it moves forward.
        const int step = std::clamp(match.captured(1).toInt(), 0, m_totalSteps);
        if (step <= m_lastStep) return;
        m_lastStep = step;
        m_percentage = std::max(m_percentage, FirstStepPercentage + step * StepPercentageSpan / m_totalSteps);
        Nukleus::GenerationProgress progress;
        progress.stage = Nukleus::GenerationProgressStage::Training;
        progress.currentStep = step;
        progress.totalSteps = m_totalSteps;
        progress.percentage = m_percentage;
        progress.statusText = tr("Training step %1 / %2").arg(step).arg(m_totalSteps);
        emit progressChanged(progress);
        return;
    }
    qInfo().noquote() << QStringLiteral("[lora-trainer][%1] %2").arg(standardError ? QStringLiteral("stderr") : QStringLiteral("stdout"), line);
    if (standardError) m_standardError = (m_standardError + line + QLatin1Char('\n')).right(ErrorTailCharacters);
}
