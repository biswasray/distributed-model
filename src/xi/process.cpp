// Backend-independent parts of ManagedProcess.
#include "xi/process.hpp"

#include <algorithm>
#include <fstream>

namespace xi {

nlohmann::json ProcInfo::to_json() const {
    nlohmann::json j{{"state", state}};
    j["pid"] = pid ? nlohmann::json(*pid) : nlohmann::json(nullptr);
    j["exit_code"] = exit_code ? nlohmann::json(*exit_code) : nlohmann::json(nullptr);
    j["uptime_s"] = uptime_s ? nlohmann::json(*uptime_s) : nlohmann::json(nullptr);
    return j;
}

ProcInfo ManagedProcess::stop(double grace_s) {
    {
        std::lock_guard lk(mu_);
        stop_locked(grace_s);
    }
    return info();
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

}  // namespace xi
