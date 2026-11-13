// Thread-safe logging to stderr and (optionally) logs\<name>.log.
#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace xgi::log {

enum class Level { debug = 0, info = 1, warn = 2, error = 3 };

// Opens <log_dir>/<name>.log for appending. An empty log_dir logs to stderr only.
void init(const std::string& name, const std::filesystem::path& log_dir, Level min_level = Level::info);
void set_level(Level level);
Level parse_level(std::string_view s);  // "debug" | "info" | "warn" | "error"
void write(Level level, std::string_view msg);

template <class... A> void debug(std::format_string<A...> f, A&&... a) { write(Level::debug, std::format(f, std::forward<A>(a)...)); }
template <class... A> void info(std::format_string<A...> f, A&&... a)  { write(Level::info,  std::format(f, std::forward<A>(a)...)); }
template <class... A> void warn(std::format_string<A...> f, A&&... a)  { write(Level::warn,  std::format(f, std::forward<A>(a)...)); }
template <class... A> void error(std::format_string<A...> f, A&&... a) { write(Level::error, std::format(f, std::forward<A>(a)...)); }

}  // namespace xgi::log
