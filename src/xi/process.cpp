#include "xi/process.hpp"

#include <windows.h>

#include <fstream>
#include <stdexcept>

#include "common/log.hpp"
#include "common/util.hpp"
#include "xi/commands.hpp"

namespace xi {

nlohmann::json ProcInfo::to_json() const {
    nlohmann::json j{{"state", state}};
    j["pid"] = pid ? nlohmann::json(*pid) : nlohmann::json(nullptr);
    j["exit_code"] = exit_code ? nlohmann::json(*exit_code) : nlohmann::json(nullptr);
    j["uptime_s"] = uptime_s ? nlohmann::json(*uptime_s) : nlohmann::json(nullptr);
    return j;
}

ManagedProcess::ManagedProcess(std::string role, std::filesystem::path log_file)
    : role_(std::move(role)), log_file_(std::move(log_file)) {}

ManagedProcess::~ManagedProcess() {
    std::lock_guard lk(mu_);
    stop_locked(5.0);
}

ProcInfo ManagedProcess::start(const std::vector<std::string>& argv) {
    std::lock_guard lk(mu_);
    stop_locked(15.0);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // Only these handles are inherited, so concurrent starts of the other role can't leak
    // their pipe ends into this child (which would keep the reader from seeing EOF).
    HANDLE inherit[2] = {write_end, nul};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError = write_end;
    si.lpAttributeList = attrs;

    DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (GetConsoleWindow() == nullptr) flags |= CREATE_NO_WINDOW;  // running as a service: no console to share

    std::wstring cmd = build_command_line(argv);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(write_end);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(read_end);
        throw std::runtime_error("cannot start " + argv.at(0) + " (Windows error " + std::to_string(err) + ")");
    }

    // Kill-on-close job: the child (and anything it spawns) dies with XI, even if XI crashes.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    process_ = pi.hProcess;
    job_ = job;
    pid_ = pi.dwProcessId;
    started_mono_ = xgi::mono_now();
    stopped_by_us_ = false;
    append_line("[xi] started pid " + std::to_string(pid_) + ": " + xgi::narrow(cmd));
    xgi::log::info("{}: started pid {}: {}", role_, pid_, xgi::narrow(cmd));
    reader_ = std::thread([this, read_end] { reader_loop(read_end); });

    ProcInfo info;
    info.state = "running";
    info.pid = pid_;
    info.uptime_s = 0.0;
    return info;
}

ProcInfo ManagedProcess::stop(double grace_s) {
    {
        std::lock_guard lk(mu_);
        stop_locked(grace_s);
    }
    return info();
}

void ManagedProcess::stop_locked(double grace_s) {
    if (process_) {
        HANDLE h = static_cast<HANDLE>(process_);
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
            xgi::log::info("{}: stopping pid {}", role_, pid_);
            bool signalled = GetConsoleWindow() != nullptr && GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_);
            if (!signalled || WaitForSingleObject(h, static_cast<DWORD>(grace_s * 1000)) == WAIT_TIMEOUT) {
                if (job_)
                    TerminateJobObject(static_cast<HANDLE>(job_), 1);
                else
                    TerminateProcess(h, 1);
                WaitForSingleObject(h, 5000);
            }
        }
        CloseHandle(h);
        process_ = nullptr;
    }
    if (job_) {
        CloseHandle(static_cast<HANDLE>(job_));  // kill-on-close takes any leftovers with it
        job_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    if (!stopped_by_us_) append_line("[xi] stopped pid " + std::to_string(pid_));
    stopped_by_us_ = true;
    pid_ = 0;
}

ProcInfo ManagedProcess::info() {
    std::lock_guard lk(mu_);
    ProcInfo i;
    if (!process_) return i;  // never started, or stopped by us
    HANDLE h = static_cast<HANDLE>(process_);
    i.pid = pid_;
    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        i.state = "running";
        i.uptime_s = xgi::mono_now() - started_mono_;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(h, &code);
        i.state = "exited";
        i.exit_code = static_cast<long long>(static_cast<int>(code));
    }
    return i;
}

std::vector<std::string> ManagedProcess::tail(size_t n) {
    std::lock_guard lk(lines_mu_);
    n = std::min(n, lines_.size());
    return {lines_.end() - static_cast<std::ptrdiff_t>(n), lines_.end()};
}

void ManagedProcess::append_line(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    {
        std::lock_guard lk(lines_mu_);
        lines_.push_back(line);
        while (lines_.size() > kTailLines) lines_.pop_front();
    }
    std::ofstream f(log_file_, std::ios::app | std::ios::binary);
    f << line << "\n";
}

void ManagedProcess::reader_loop(void* read_handle) {
    HANDLE h = static_cast<HANDLE>(read_handle);
    std::string pending;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            append_line(pending.substr(0, pos));
            pending.erase(0, pos + 1);
        }
    }
    if (!pending.empty()) append_line(pending);
    CloseHandle(h);
}

}  // namespace xi
