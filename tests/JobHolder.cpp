#include "ChildProcessJob.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTextStream>
#include <QThread>

// Test helper standing in for the daemon: it ties its children to itself, starts a long-running child,
// prints the child's process id and waits to be killed. Windows only; it does not run anything else.
int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (!endChildrenWithThisProcess()) return 2;
    QProcess child;
    child.setStandardOutputFile(QProcess::nullDevice());
    child.start(QStringLiteral("ping"), {QStringLiteral("-n"), QStringLiteral("120"), QStringLiteral("127.0.0.1")});
    if (!child.waitForStarted()) return 3;
    QTextStream(stdout) << child.processId() << Qt::endl;
    for (;;) QThread::sleep(1);
}
