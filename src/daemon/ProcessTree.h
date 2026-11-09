#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

class QProcess;

// Owns the processes of one external tool run: the process QProcess starts and everything it starts in turn,
// so that stopping the run leaves nothing behind (a Python virtual environment's launcher starts the real
// interpreter as its own child). Windows puts the process in a job object of its own, nested in the daemon's;
// Linux starts it in a process group of its own.
//
// Use: prepare() before QProcess::start(), attach() from the started() signal, stop() to end the whole tree.
// ended() is emitted once after stop(), when no process of the tree remains.
class ProcessTree final : public QObject
{
    Q_OBJECT

public:
    explicit ProcessTree(QObject *parent = nullptr);
    ~ProcessTree() override;

    // Makes the process start in a tree of its own. Call before start().
    void prepare(QProcess &process);
    // Takes the started process into the tree, before it can start a child. Returns false, with the reason in
    // error, when it could not; the caller then ends the process itself.
    bool attach(const QProcess &process, QString *error = nullptr);
    // True while a process of the tree remains.
    bool isRunning() const;
    // Ends every process of the tree and, once none remains, emits ended(). Linux sends SIGTERM to the group,
    // then SIGKILL after a short grace period; Windows ends the job object at once.
    void stop();

signals:
    void ended();

private:
    void poll();
    void terminate(bool force);

    QTimer m_poll;
    QElapsedTimer m_stopClock;
    bool m_stopping = false;
    bool m_forced = false;
#ifdef Q_OS_WIN
    void *m_job = nullptr;
#else
    qint64 m_group = 0;
#endif
};
