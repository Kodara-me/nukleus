#pragma once

#include <QString>
#include <QtGlobal>

#include <memory>

namespace NukleusService
{
#ifdef Q_OS_WIN
// Lets an executable run under the Windows Service Control Manager (SCM) or from a console, with the same code.
// Create it right after the QCoreApplication: when the SCM started the process it reports the service's states,
// and a Stop or shutdown request quits the application's event loop (on the main thread). From a console every
// call does nothing, so the executable behaves as before.
class ServiceHost
{
public:
    explicit ServiceHost(const QString &serviceName);
    ~ServiceHost();
    ServiceHost(const ServiceHost &) = delete;
    ServiceHost &operator=(const ServiceHost &) = delete;

    // True when the SCM started this process.
    bool isService() const;
    // Reports Start Pending, until running() or failed().
    void startPending();
    // Reports Running: the service accepts connections.
    void running();
    // Reports Stopped with a nonzero service-specific exit code, and returns that code for `main` to return.
    int failed(int exitCode);
    // Reports Stopped after a requested stop, once the service's objects are gone.
    void stopped();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
#else
// Outside Windows there is no SCM: a console start only.
class ServiceHost
{
public:
    explicit ServiceHost(const QString &) {}
    bool isService() const { return false; }
    void startPending() {}
    void running() {}
    int failed(int exitCode) { return exitCode; }
    void stopped() {}
};
#endif
}
