// Small helpers shared by XGI and XI.
#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace xgi {

inline constexpr const char* kVersion = XGI_VERSION;
inline constexpr const char* kTokenEnv = "QWEN_CLUSTER_TOKEN";
inline constexpr const char* kTokenHeader = "X-Cluster-Token";

// Wall-clock seconds since the Unix epoch (for timestamps in the API).
double wall_now();
// Monotonic seconds (for timeouts and intervals).
double mono_now();

std::optional<std::string> get_env(const char* name);

// Constant-time comparison for secrets.
bool secure_equals(std::string_view a, std::string_view b);

std::string to_lower(std::string s);
bool ends_with_ci(std::string_view s, std::string_view suffix);
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Formats a double the shortest way that round-trips ("1", "0.5"), as llama.cpp expects.
std::string format_number(double v);

// Sleeps for `seconds` or until stop is requested. Returns false if stopped.
bool interruptible_sleep(std::stop_token st, double seconds);

// A flag a thread can wait on with a timeout (used for shutdown).
class StopFlag {
public:
    void set();
    bool is_set() const;
    // Waits up to `seconds`. Returns true if the flag was set.
    bool wait_for(double seconds);

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool set_ = false;
};

}  // namespace xgi
