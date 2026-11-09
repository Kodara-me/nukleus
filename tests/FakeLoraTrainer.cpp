// Stand-in for the LoRA trainer's Python (kohya-ss/sd-scripts' virtual environment), for the daemon's tests.
//
// A test lays out a trainer directory with a file named sdxl_train_network.py and copies this executable to
// venv/bin/python (Linux) or venv\Scripts\python.exe (Windows). The daemon runs it as it would run Python:
//
//   python --version
//   python <trainer>/sdxl_train_network.py <arguments...>
//
// The trainer directory is three levels above this executable. The test tells the fake what to do in
// <trainer>/control-<output name>.txt, or else <trainer>/control.txt: one key=value per line.
//
//   versionExit=N     exit code of --version (default 0)
//   exit=N            exit code of a training run (default 0)
//   output=yes|no|directory   write <output_dir>/<output_name>.safetensors (default), nothing, or a directory
//   progress=a,b,c    steps for which a tqdm line is printed (default 1,2)
//   delayMs=N         pause after each tqdm line (default 5)
//   error=TEXT        printed to the error output before the end
//   child=yes         start a child process that records its process ID in <trainer>/child-<name>.pid and
//                     keeps running, standing in for the venv launcher's interpreter
//   hold=yes          after the progress, wait until <trainer>/release-<name> exists
//
// What the run received is recorded in <trainer>/record-<name>.json when it starts: its arguments, working
// directory, environment and the files in the working directory, as hexadecimal. <trainer>/started-<name>
// appears once the record is complete.
//
// Standard C++ and the system calls only, so that a copy of the executable runs wherever the tests put it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
std::string selfPath()
{
#ifdef _WIN32
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    return fs::path(std::wstring(buffer, length)).string();
#else
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    return length > 0 ? std::string(buffer, size_t(length)) : std::string();
#endif
}

long processId()
{
#ifdef _WIN32
    return long(GetCurrentProcessId());
#else
    return long(getpid());
#endif
}

void sleepMs(long milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::string readFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

std::string hex(const std::string &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

std::string json(const std::string &text)
{
    std::string result = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            result.push_back('\\');
            result.push_back(c);
        } else if (c == '\n') {
            result += "\\n";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            result += "?";
        } else {
            result.push_back(c);
        }
    }
    return result + "\"";
}

std::map<std::string, std::string> readControl(const fs::path &trainer, const std::string &name)
{
    fs::path path = trainer / ("control-" + name + ".txt");
    if (!fs::exists(path)) path = trainer / "control.txt";
    std::map<std::string, std::string> control;
    std::istringstream lines(readFile(path));
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t equals = line.find('=');
        if (equals != std::string::npos) control[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return control;
}

std::string value(const std::map<std::string, std::string> &control, const std::string &key, const std::string &fallback)
{
    const auto found = control.find(key);
    return found == control.end() ? fallback : found->second;
}

// Starts this executable again as the idle child, in the caller's job object or process group.
void startIdleChild(const std::string &self, const fs::path &pidFile)
{
#ifdef _WIN32
    std::wstring command = L"\"" + fs::path(self).wstring() + L"\" --idle \"" + pidFile.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information)) {
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
    }
#else
    if (fork() == 0) {
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        execl(self.c_str(), self.c_str(), "--idle", pidFile.string().c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
#endif
    for (int attempt = 0; attempt < 500 && !fs::exists(pidFile); ++attempt) sleepMs(10);
}
}

int main(int argc, char **argv)
{
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.size() == 2 && arguments.front() == "--idle") {
        writeFile(arguments.back() + ".tmp", std::to_string(processId()));
        fs::rename(arguments.back() + ".tmp", arguments.back());
        for (;;) sleepMs(100);
    }
    const std::string self = selfPath();
    const fs::path trainer = fs::path(self).parent_path().parent_path().parent_path();
    if (arguments.size() == 1 && arguments.front() == "--version") {
        std::puts("Python 3.12.10");
        return std::atoi(value(readControl(trainer, ""), "versionExit", "0").c_str());
    }
    if (arguments.empty()) return 2;

    // The arguments after the script: --name value pairs and bare flags.
    std::map<std::string, std::string> options;
    for (size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index].rfind("--", 0) != 0) continue;
        const bool hasValue = index + 1 < arguments.size() && arguments[index + 1].rfind("--", 0) != 0;
        options[arguments[index].substr(2)] = hasValue ? arguments[index + 1] : "";
        if (hasValue) ++index;
    }
    const std::string name = options["output_name"];
    const auto control = readControl(trainer, name);

    // What the daemon gave this run.
    std::ostringstream record;
    record << "{\"pid\":" << processId() << ",\"script\":" << json(arguments.front()) << ",\"cwd\":" << json(fs::current_path().string())
           << ",\"arguments\":[";
    for (size_t index = 1; index < arguments.size(); ++index) record << (index > 1 ? "," : "") << json(arguments[index]);
    record << "],\"environment\":{";
    bool first = true;
    for (const char *variable : {"HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE", "PYTHONDONTWRITEBYTECODE", "HOME", "HF_HOME",
                                 "XDG_CACHE_HOME", "TORCHINDUCTOR_CACHE_DIR", "TRITON_CACHE_DIR", "MPLCONFIGDIR",
                                 "MIOPEN_USER_DB_PATH", "MIOPEN_CUSTOM_CACHE_DIR"}) {
        const char *content = std::getenv(variable);
        if (!content) continue;
        record << (first ? "" : ",") << json(variable) << ":" << json(content);
        first = false;
    }
    record << "},\"files\":{";
    first = true;
    for (const auto &entry : fs::recursive_directory_iterator(fs::current_path())) {
        if (!entry.is_regular_file()) continue;
        record << (first ? "" : ",") << json(fs::relative(entry.path(), fs::current_path()).generic_string()) << ":"
               << json(hex(readFile(entry.path())));
        first = false;
    }
    record << "}}";
    writeFile(trainer / ("record-" + name + ".json"), record.str());

    if (value(control, "child", "no") == "yes") startIdleChild(self, trainer / ("child-" + name + ".pid"));
    writeFile(trainer / ("started-" + name), "started");

    std::puts("fake trainer: preparing");
    std::fflush(stdout);
    const long total = std::atol(options["max_train_steps"].c_str());
    std::istringstream steps(value(control, "progress", "1,2"));
    for (std::string step; std::getline(steps, step, ',');) {
        const long current = std::atol(step.c_str());
        const long percent = total > 0 ? current * 100 / total : 0;
        std::fprintf(stderr, "\rsteps: %3ld%%|##########| %ld/%ld [00:01<00:01, 1.00it/s, avr_loss=0.1]", percent, current, total);
        std::fflush(stderr);
        sleepMs(std::atol(value(control, "delayMs", "5").c_str()));
    }
    std::fprintf(stderr, "\n");
    if (value(control, "hold", "no") == "yes")
        while (!fs::exists(trainer / ("release-" + name))) sleepMs(20);
    const std::string error = value(control, "error", "");
    if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
    std::fflush(stderr);

    const std::string output = value(control, "output", "yes");
    const fs::path outputDirectory = fs::current_path() / options["output_dir"];
    if (output != "no") {
        fs::create_directories(outputDirectory);
        if (output == "directory") fs::create_directories(outputDirectory / (name + ".safetensors"));
        else writeFile(outputDirectory / (name + ".safetensors"), "trained:" + name);
    }
    return std::atoi(value(control, "exit", "0").c_str());
}
