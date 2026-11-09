#include "ProcessTree.h"

#include <QDebug>
#include <QProcess>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <QDir>
#include <QFile>
#endif
#endif

namespace
{
constexpr int PollIntervalMs = 20;
// SIGTERM's grace period before the whole group is killed (Linux).
constexpr qint64 TerminateGraceMs = 3000;
// A process that survives SIGKILL for this long is stuck in the kernel; stop waiting for it.
constexpr qint64 GiveUpMs = 30000;

#ifdef Q_OS_LINUX
// True when a process of the group is alive. kill(-pgid, 0) also succeeds while the group only holds
// zombies, which stay until their parent reaps them; a parent that never does (a container's PID 1) would
// keep the group "running" forever, so every process of the group is looked at: only a state other than Z counts.
bool groupHasLiveProcess(pid_t group)
{
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        entry.toLongLong(&numeric);
        if (!numeric) continue;
        QFile file(QStringLiteral("/proc/%1/stat").arg(entry));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray line = file.readLine();
        // "pid (comm) state ppid pgrp ...": the name may hold spaces and parentheses, so start after the last ')'.
        const qsizetype close = line.lastIndexOf(')');
        if (close < 0) continue;
        const QList<QByteArray> fields = line.mid(close + 1).trimmed().split(' ');
        if (fields.size() < 3) continue;
        if (fields.at(0) == "Z" || fields.at(0) == "X") continue;
        if (fields.at(2).toLongLong() == static_cast<qint64>(group)) return true;
    }
    return false;
}
#endif
}

ProcessTree::ProcessTree(QObject *parent)
    : QObject(parent)
{
    m_poll.setInterval(PollIntervalMs);
    connect(&m_poll, &QTimer::timeout, this, &ProcessTree::poll);
}

ProcessTree::~ProcessTree()
{
    m_poll.stop();
    // Nothing of the tree outlives its owner: the job object kills on close, the group is killed here.
#ifdef Q_OS_WIN
    if (m_job) {
        TerminateJobObject(static_cast<HANDLE>(m_job), 1);
        CloseHandle(static_cast<HANDLE>(m_job));
    }
#else
    if (isRunning()) ::kill(-static_cast<pid_t>(m_group), SIGKILL);
#endif
}

void ProcessTree::prepare(QProcess &process)
{
#ifdef Q_OS_WIN
    // Started suspended, so that the process joins the job before it can start a child.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_SUSPENDED;
    });
#else
    process.setChildProcessModifier([] {
        ::setpgid(0, 0);
#ifdef Q_OS_LINUX
        // A crashed daemon takes the direct child with it; the rest of the group ends with the service's
        // control group.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    });
#endif
}

bool ProcessTree::attach(const QProcess &process, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const qint64 processId = process.processId();
    if (processId <= 0) return fail(tr("The process has no identifier"));
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return fail(tr("Could not create a job object (error %1)").arg(GetLastError()));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD code = GetLastError();
        CloseHandle(job);
        return fail(tr("Could not configure a job object (error %1)").arg(code));
    }
    HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(processId));
    if (!handle) {
        const DWORD code = GetLastError();
        CloseHandle(job);
        return fail(tr("Could not open the process (error %1)").arg(code));
    }
    const bool assigned = AssignProcessToJobObject(job, handle);
    const DWORD assignError = assigned ? 0 : GetLastError();
    CloseHandle(handle);
    if (!assigned) {
        CloseHandle(job);
        return fail(tr("Could not assign the process to a job object (error %1)").arg(assignError));
    }
    m_job = job;
    // Qt gives no handle to the process's main thread, so the process is resumed through its threads. A
    // process created suspended has the one, still suspended.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return fail(tr("Could not list the process's threads (error %1)").arg(GetLastError()));
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool resumed = false;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != static_cast<DWORD>(processId)) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (ResumeThread(thread) != static_cast<DWORD>(-1)) resumed = true;
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    if (!resumed) return fail(tr("Could not resume the process"));
    return true;
#else
    // The child made itself the leader of a group of its own before executing: its group is its identifier.
    m_group = processId;
    return true;
#endif
}

bool ProcessTree::isRunning() const
{
#ifdef Q_OS_WIN
    if (!m_job) return false;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
    if (!QueryInformationJobObject(static_cast<HANDLE>(m_job), JobObjectBasicAccountingInformation, &information,
                                   sizeof(information), nullptr))
        return true;
    return information.ActiveProcesses > 0;
#else
    if (m_group <= 0) return false;
    const bool exists = ::kill(-static_cast<pid_t>(m_group), 0) == 0 || errno == EPERM;
#ifdef Q_OS_LINUX
    return exists && groupHasLiveProcess(static_cast<pid_t>(m_group));
#else
    return exists;
#endif
#endif
}

void ProcessTree::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    m_stopClock.start();
    terminate(false);
    m_poll.start();
}

void ProcessTree::terminate(bool force)
{
#ifdef Q_OS_WIN
    Q_UNUSED(force)
    if (m_job) TerminateJobObject(static_cast<HANDLE>(m_job), 1);
#else
    // An empty group is left alone: its number may be another group's by now.
    if (isRunning()) ::kill(-static_cast<pid_t>(m_group), force ? SIGKILL : SIGTERM);
#endif
}

void ProcessTree::poll()
{
    if (!isRunning()) {
        m_poll.stop();
#ifndef Q_OS_WIN
        m_group = 0;
#endif
        emit ended();
        return;
    }
    const qint64 elapsed = m_stopClock.elapsed();
    if (!m_forced && elapsed >= TerminateGraceMs) {
        m_forced = true;
        terminate(true);
    }
    if (elapsed >= GiveUpMs) {
        qWarning().noquote() << QStringLiteral("[Nukleus] A process of a stopped tool run did not end after %1 s.").arg(GiveUpMs / 1000);
        m_poll.stop();
        emit ended();
    }
}
