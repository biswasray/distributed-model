// Supervises one llama.cpp child process (02-node-agent.md §5).
#pragma once

#include <deque>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace xi {

struct ProcInfo {
    std::string state = "stopped";  // stopped | running | exited
    std::optional<unsigned long> pid;
    std::optional<long long> exit_code;
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

    // Starts argv[0] with argv[1..]. A running process is stopped first (restart semantics).
    // Throws std::runtime_error if the process cannot be created.
    ProcInfo start(const std::vector<std::string>& argv);
    // Graceful stop: CTRL_BREAK (when the console is shared), wait up to grace_s, then kill the job.
    ProcInfo stop(double grace_s = 15.0);
    ProcInfo info();
    std::vector<std::string> tail(size_t n);
    const std::string& role() const { return role_; }

private:
    void stop_locked(double grace_s);
    void reader_loop(void* read_handle);
    void append_line(std::string line);

    std::string role_;
    std::filesystem::path log_file_;

    std::mutex mu_;          // guards process state
    void* process_ = nullptr;  // HANDLE
    void* job_ = nullptr;      // HANDLE
    unsigned long pid_ = 0;
    double started_mono_ = 0;
    bool stopped_by_us_ = true;
    std::thread reader_;

    std::mutex lines_mu_;
    std::deque<std::string> lines_;
};

}  // namespace xi
