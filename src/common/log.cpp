#include "common/log.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
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
    auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    auto local = std::chrono::zoned_time{std::chrono::current_zone(), now};
    std::string line = std::format("{:%Y-%m-%d %H:%M:%S} {:<5} {}: {}\n", local, level_name(level), g_name, msg);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    if (g_file.is_open()) {
        g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        g_file.flush();
    }
}

}  // namespace xgi::log
