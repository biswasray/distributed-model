// POSIX backend for ManagedProcess (Linux, macOS): fork/exec into a new process group.
//
// - The child gets its own process group (like CREATE_NEW_PROCESS_GROUP), so a terminal
//   Ctrl+C reaches only XI; XI then stops its children itself.
// - "Terminate" = SIGTERM to the group, wait up to the grace period, then SIGKILL the group.
// - Linux: PR_SET_PDEATHSIG kills the child if XI dies. macOS has no equivalent; there, a
//   crashed XI can leave a llama.cpp process running until it is stopped by hand.
#include "xi/process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "common/log.hpp"
#include "common/util.hpp"

namespace xi {

struct ManagedProcess::Native {
    bool active = false;  // a child exists that we haven't stopped
    bool reaped = false;
    long long exit_code = 0;
};

namespace {

void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

long long decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

}  // namespace

ManagedProcess::ManagedProcess(std::string role, std::filesystem::path log_file)
    : role_(std::move(role)), log_file_(std::move(log_file)), native_(std::make_unique<Native>()) {}

ManagedProcess::~ManagedProcess() {
    std::lock_guard lk(mu_);
    stop_locked(5.0);
}

ProcInfo ManagedProcess::start(const std::vector<std::string>& argv_in) {
    std::lock_guard lk(mu_);
    stop_locked(15.0);
    if (argv_in.empty()) throw std::runtime_error("empty command line");

    int out[2], err_pipe[2];
    if (::pipe(out) != 0) throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    if (::pipe(err_pipe) != 0) {
        ::close(out[0]);
        ::close(out[1]);
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }
    set_cloexec(out[0]);
    set_cloexec(err_pipe[0]);
    set_cloexec(err_pipe[1]);  // closes on successful exec -> parent reads EOF

    // Everything the child needs is prepared before fork(): after fork only async-signal-safe calls.
    std::vector<std::string> args = argv_in;
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    const pid_t parent = ::getpid();
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;

    pid_t pid = ::fork();
    if (pid < 0) {
        int e = errno;
        ::close(out[0]); ::close(out[1]); ::close(err_pipe[0]); ::close(err_pipe[1]);
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(e));
    }
    if (pid == 0) {
        // ---- child
        ::setpgid(0, 0);
#if defined(__linux__)
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent) ::_exit(127);  // parent already gone
#endif
        (void)parent;
        ::signal(SIGPIPE, SIG_DFL);  // XI ignores SIGPIPE; llama.cpp should not inherit that
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, 0);
        ::dup2(out[1], 1);
        ::dup2(out[1], 2);
        // Close every other inherited descriptor (e.g. XI's listening socket).
        for (int fd = 3; fd < max_fd; ++fd)
            if (fd != err_pipe[1]) ::close(fd);
        ::execvp(cargv[0], cargv.data());
        int e = errno;
        ssize_t w = ::write(err_pipe[1], &e, sizeof(e));
        (void)w;
        ::_exit(127);
    }

    // ---- parent
    ::setpgid(pid, pid);  // also set here to avoid a race with the child
    ::close(out[1]);
    ::close(err_pipe[1]);
    int child_errno = 0;
    ssize_t n;
    do {
        n = ::read(err_pipe[0], &child_errno, sizeof(child_errno));
    } while (n < 0 && errno == EINTR);
    ::close(err_pipe[0]);
    if (n == static_cast<ssize_t>(sizeof(child_errno))) {
        ::close(out[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        throw std::runtime_error("cannot start " + args[0] + ": " + std::strerror(child_errno));
    }

    native_->active = true;
    native_->reaped = false;
    pid_ = pid;
    started_mono_ = xgi::mono_now();
    stopped_by_us_ = false;
    std::string cmd = xgi::join(args, " ");
    append_line("[xi] started pid " + std::to_string(pid_) + ": " + cmd);
    xgi::log::info("{}: started pid {}: {}", role_, pid_, cmd);
    int read_fd = out[0];
    reader_ = std::thread([this, read_fd] { reader_loop(read_fd); });

    ProcInfo info;
    info.state = "running";
    info.pid = pid_;
    info.uptime_s = 0.0;
    return info;
}

void ManagedProcess::stop_locked(double grace_s) {
    if (native_->active) {
        const pid_t pid = static_cast<pid_t>(pid_);
        auto try_reap = [&] {
            if (native_->reaped) return true;
            int status = 0;
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) {
                native_->reaped = true;
                native_->exit_code = decode_status(status);
            }
            return native_->reaped;
        };
        if (!try_reap()) {
            xgi::log::info("{}: stopping pid {}", role_, pid_);
            ::kill(-pid, SIGTERM);
            double deadline = xgi::mono_now() + grace_s;
            while (!try_reap() && xgi::mono_now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!native_->reaped) {
                ::kill(-pid, SIGKILL);
                int status = 0;
                ::waitpid(pid, &status, 0);
                native_->reaped = true;
                native_->exit_code = decode_status(status);
            }
        }
        ::kill(-pid, SIGKILL);  // anything left in the group
        native_->active = false;
    }
    if (reader_.joinable()) reader_.join();
    if (!stopped_by_us_) append_line("[xi] stopped pid " + std::to_string(pid_));
    stopped_by_us_ = true;
    pid_ = 0;
}

ProcInfo ManagedProcess::info() {
    std::lock_guard lk(mu_);
    ProcInfo i;
    if (!native_->active) return i;  // never started, or stopped by us
    i.pid = pid_;
    if (!native_->reaped) {
        int status = 0;
        pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        if (r == static_cast<pid_t>(pid_)) {
            native_->reaped = true;
            native_->exit_code = decode_status(status);
        }
    }
    if (native_->reaped) {
        i.state = "exited";
        i.exit_code = native_->exit_code;
    } else {
        i.state = "running";
        i.uptime_s = xgi::mono_now() - started_mono_;
    }
    return i;
}

void ManagedProcess::reader_loop(std::intptr_t read_handle) {
    int fd = static_cast<int>(read_handle);
    std::string pending;
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        pending.append(buf, static_cast<size_t>(n));
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            append_line(pending.substr(0, pos));
            pending.erase(0, pos + 1);
        }
    }
    if (!pending.empty()) append_line(pending);
    ::close(fd);
}

}  // namespace xi
