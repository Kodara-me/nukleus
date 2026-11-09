#pragma once

#include <nukleus/GenerationProgress.h>

#include <QMetaType>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <memory>

class ProcessTree;
class QProcess;

struct LoraTrainerResult
{
    bool succeeded = false;
    bool canceled = false;
    int exitCode = -1;
    QString command; // The command line that ran.
    QString standardError; // Its trailing error output.
};
Q_DECLARE_METATYPE(LoraTrainerResult)

// Runs kohya-ss/sd-scripts' sdxl_train_network.py with the Python of the virtual environment installed beside
// it, in a process tree of its own (ProcessTree) that is ended with the run. Reads the progress bar of the
// training steps.
class LoraTrainerRunner final : public QObject
{
    Q_OBJECT

public:
    explicit LoraTrainerRunner(QString trainerDirectory, QObject *parent = nullptr);
    ~LoraTrainerRunner() override;

    QString directory() const;
    QString scriptPath() const;
    QString pythonPath() const;
    // True when the directory holds the script and a Python that exits with code 0 on --version. Runs the
    // Python once and waits for it, so call it at startup only.
    bool isReady() const;
    // Starts the trainer: `script` and then `arguments`, with workingDirectory as its working directory. The
    // progress reports step counts out of totalSteps. Returns false when a run is in progress.
    bool run(const QStringList &arguments, const QString &workingDirectory, const QProcessEnvironment &environment,
             int totalSteps);
    // Stops the run's whole process tree; finished() follows with canceled set.
    void cancel();
    bool isRunning() const;
    QString commandLine(const QStringList &arguments) const;

signals:
    // Training steps only; the stages around them are the caller's.
    void progressChanged(const Nukleus::GenerationProgress &progress);
    // Emitted once per run, after the trainer and every process it started have ended.
    void finished(const LoraTrainerResult &result);

private:
    void readOutput(bool standardError);
    void processLine(const QString &line, bool standardError);
    void finishIfDone();
    void processFinished(int exitCode, bool normalExit);

    QString m_directory;
    QProcess *m_process = nullptr;
    ProcessTree *m_tree = nullptr;
    QStringList m_arguments;
    int m_totalSteps = 0;
    int m_lastStep = -1;
    int m_percentage = 0;
    bool m_running = false;
    bool m_canceled = false;
    bool m_processDone = false;
    bool m_treeEnded = false;
    bool m_reported = false;
    bool m_attachFailed = false;
    int m_exitCode = -1;
    bool m_normalExit = false;
    QString m_pendingOutput;
    QString m_pendingError;
    QString m_standardError;
};
