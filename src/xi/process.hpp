// Supervises one llama.cpp child process (02-node-agent.md §5).
// Backends: process_windows.cpp (job objects) and process_posix.cpp (fork/exec, process groups).
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace xi {

struct ProcInfo {
    std::string state = "stopped";  // stopped | running | exited
    std::optional<long long> pid;
    std::optional<long long> exit_code;  // POSIX: negative signal number if killed by a signal
    std::optional<double> uptime_s;
    nlohmann::json to_json() const;
};

class ManagedProcess {
public:
    static constexpr size_t kTailLines = 500;

    ManagedProcess(std::string role, std::filesystem::path log_file);
    ~ManagedProcess();
    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    // Starts argv[0] (searched on PATH if it has no directory) with argv[1..].
    // A running process is stopped first (restart semantics). Throws std::runtime_error on failure.
    ProcInfo start(const std::vector<std::string>& argv);
    // Graceful stop: CTRL_BREAK / SIGTERM to the process group, wait up to grace_s, then kill.
    ProcInfo stop(double grace_s = 15.0);
    ProcInfo info();
    std::vector<std::string> tail(size_t n);
    const std::string& role() const { return role_; }

private:
    struct Native;  // OS handles, defined by the backend

    void stop_locked(double grace_s);
    void append_line(std::string line);
    void reader_loop(std::intptr_t read_handle);  // reads merged stdout/stderr until EOF

    std::string role_;
    std::filesystem::path log_file_;

    std::mutex mu_;  // guards process state
    std::unique_ptr<Native> native_;
    long long pid_ = 0;
    double started_mono_ = 0;
    bool stopped_by_us_ = true;
    std::thread reader_;

    std::mutex lines_mu_;
    std::deque<std::string> lines_;
};

}  // namespace xi
