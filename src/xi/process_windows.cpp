// Windows backend for ManagedProcess: CreateProcessW + kill-on-close job objects.
#include "xi/process.hpp"

#include <windows.h>

#include <stdexcept>

#include "common/log.hpp"
#include "common/util.hpp"
#include "xi/commands.hpp"

namespace xi {

struct ManagedProcess::Native {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
};

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

ManagedProcess::ManagedProcess(std::string role, std::filesystem::path log_file)
    : role_(std::move(role)), log_file_(std::move(log_file)), native_(std::make_unique<Native>()) {}

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

    std::string cmd_utf8 = build_command_line(argv);
    std::wstring cmd = widen(cmd_utf8);
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

    native_->process = pi.hProcess;
    native_->job = job;
    pid_ = static_cast<long long>(pi.dwProcessId);
    started_mono_ = xgi::mono_now();
    stopped_by_us_ = false;
    append_line("[xi] started pid " + std::to_string(pid_) + ": " + cmd_utf8);
    xgi::log::info("{}: started pid {}: {}", role_, pid_, cmd_utf8);
    reader_ = std::thread([this, read_end] { reader_loop(reinterpret_cast<std::intptr_t>(read_end)); });

    ProcInfo info;
    info.state = "running";
    info.pid = pid_;
    info.uptime_s = 0.0;
    return info;
}

void ManagedProcess::stop_locked(double grace_s) {
    if (native_->process) {
        HANDLE h = native_->process;
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
            xgi::log::info("{}: stopping pid {}", role_, pid_);
            bool signalled = GetConsoleWindow() != nullptr &&
                             GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid_));
            if (!signalled || WaitForSingleObject(h, static_cast<DWORD>(grace_s * 1000)) == WAIT_TIMEOUT) {
                if (native_->job)
                    TerminateJobObject(native_->job, 1);
                else
                    TerminateProcess(h, 1);
                WaitForSingleObject(h, 5000);
            }
        }
        CloseHandle(h);
        native_->process = nullptr;
    }
    if (native_->job) {
        CloseHandle(native_->job);  // kill-on-close takes any leftovers with it
        native_->job = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    if (!stopped_by_us_) append_line("[xi] stopped pid " + std::to_string(pid_));
    stopped_by_us_ = true;
    pid_ = 0;
}

ProcInfo ManagedProcess::info() {
    std::lock_guard lk(mu_);
    ProcInfo i;
    if (!native_->process) return i;  // never started, or stopped by us
    HANDLE h = native_->process;
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

void ManagedProcess::reader_loop(std::intptr_t read_handle) {
    HANDLE h = reinterpret_cast<HANDLE>(read_handle);
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
