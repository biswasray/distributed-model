#include "common/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace xgi::log {
namespace {

std::mutex g_mu;
std::ofstream g_file;
std::string g_name = "xgi";
Level g_min = Level::info;

const char* level_name(Level l) {
    switch (l) {
        case Level::debug: return "DEBUG";
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
    }
    return "?";
}

// Local time "YYYY-MM-DD HH:MM:SS.mmm" without <chrono> time zones (not in every libc++).
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::format("{}.{:03}", buf, ms);
}

}  // namespace

void init(const std::string& name, const std::filesystem::path& log_dir, Level min_level) {
    std::lock_guard lk(g_mu);
    g_name = name;
    g_min = min_level;
    if (!log_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        g_file.open(log_dir / (name + ".log"), std::ios::app | std::ios::binary);
    }
}

void set_level(Level level) {
    std::lock_guard lk(g_mu);
    g_min = level;
}

Level parse_level(std::string_view s) {
    if (s == "debug") return Level::debug;
    if (s == "warn" || s == "warning") return Level::warn;
    if (s == "error") return Level::error;
    return Level::info;
}

void write(Level level, std::string_view msg) {
    std::lock_guard lk(g_mu);
    if (level < g_min) return;
    std::string line = std::format("{} {:<5} {}: {}\n", timestamp(), level_name(level), g_name, msg);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    if (g_file.is_open()) {
        g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        g_file.flush();
    }
}

}  // namespace xgi::log
