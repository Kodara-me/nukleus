#include "ServiceHost.h"

#include <QCoreApplication>
#include <QMetaObject>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace NukleusService
{
namespace
{
constexpr DWORD StartWaitHintMs = 30000;
constexpr DWORD StopWaitHintMs = 20000; // the time the SCM is told a stop can take (R10)
}

class ServiceHost::Impl
{
public:
    explicit Impl(const QString &serviceName)
        : name(serviceName.toStdWString())
    {
        current = this;
        finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // The dispatcher blocks until the service ends, so it runs on its own thread, and the Qt event loop
        // keeps the main thread. Outside the SCM it fails at once, which tells a console start.
        dispatcher = std::thread([this] {
            SERVICE_TABLE_ENTRYW table[] = {{name.data(), &Impl::serviceMain}, {nullptr, nullptr}};
            if (!StartServiceCtrlDispatcherW(table)) decide(false);
        });
        std::unique_lock<std::mutex> lock(mutex);
        decision.wait(lock, [this] { return decided; });
    }

    ~Impl()
    {
        if (service && !ended) report(SERVICE_STOPPED, NO_ERROR, 0, 0); // never left running or pending
        if (finished) SetEvent(finished);
        if (dispatcher.joinable()) dispatcher.join();
        if (finished) CloseHandle(finished);
        current = nullptr;
    }

    bool isService() const { return service; }

    // Sets the state the SCM sees. Pending states accept no control and carry a checkpoint and a wait hint.
    void report(DWORD state, DWORD win32ExitCode, DWORD specificExitCode, DWORD waitHintMs)
    {
        if (!service || !handle) return;
        std::lock_guard<std::mutex> lock(statusMutex);
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = state;
        status.dwWin32ExitCode = win32ExitCode;
        status.dwServiceSpecificExitCode = specificExitCode;
        const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
        status.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
        status.dwCheckPoint = pending ? ++checkpoint : 0;
        status.dwWaitHint = pending ? waitHintMs : 0;
        lastState = state;
        lastStatus = status;
        SetServiceStatus(handle, &status);
    }

    void finish()
    {
        ended = true;
        SetEvent(finished);
        if (dispatcher.joinable()) dispatcher.join();
    }

    static Impl *current;
    std::wstring name;
    bool service = false;

private:
    void decide(bool isServiceStart)
    {
        std::lock_guard<std::mutex> lock(mutex);
        service = isServiceStart;
        decided = true;
        decision.notify_all();
    }

    static void WINAPI serviceMain(DWORD, LPWSTR *)
    {
        Impl *self = current;
        if (!self) return;
        self->handle = RegisterServiceCtrlHandlerExW(self->name.c_str(), &Impl::control, self);
        if (!self->handle) {
            self->decide(false);
            return;
        }
        self->decide(true);
        self->report(SERVICE_START_PENDING, NO_ERROR, 0, StartWaitHintMs);
        // The service ends when the main thread reports Stopped; returning earlier would end the process.
        WaitForSingleObject(self->finished, INFINITE);
    }

    static DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID context)
    {
        auto *self = static_cast<Impl *>(context);
        switch (code) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            self->report(SERVICE_STOP_PENDING, NO_ERROR, 0, StopWaitHintMs);
            // The listeners and the running job belong to the main thread: it stops them when its loop ends.
            if (QCoreApplication *application = QCoreApplication::instance())
                QMetaObject::invokeMethod(application, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE: {
            std::lock_guard<std::mutex> lock(self->statusMutex);
            if (self->handle) SetServiceStatus(self->handle, &self->lastStatus);
            return NO_ERROR;
        }
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    std::thread dispatcher;
    std::mutex mutex;
    std::condition_variable decision;
    bool decided = false;
    bool ended = false;
    HANDLE finished = nullptr;
    SERVICE_STATUS_HANDLE handle = nullptr;
    std::mutex statusMutex;
    SERVICE_STATUS lastStatus{};
    DWORD lastState = SERVICE_START_PENDING;
    DWORD checkpoint = 0;
};

ServiceHost::Impl *ServiceHost::Impl::current = nullptr;

ServiceHost::ServiceHost(const QString &serviceName)
    : m_impl(std::make_unique<Impl>(serviceName))
{
}

ServiceHost::~ServiceHost() = default;

bool ServiceHost::isService() const
{
    return m_impl->isService();
}

void ServiceHost::startPending()
{
    m_impl->report(SERVICE_START_PENDING, NO_ERROR, 0, StartWaitHintMs);
}

void ServiceHost::running()
{
    m_impl->report(SERVICE_RUNNING, NO_ERROR, 0, 0);
}

int ServiceHost::failed(int exitCode)
{
    m_impl->report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(exitCode), 0);
    m_impl->finish();
    return exitCode;
}

void ServiceHost::stopped()
{
    m_impl->report(SERVICE_STOPPED, NO_ERROR, 0, 0);
    m_impl->finish();
}
}
