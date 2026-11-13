// Small helpers shared by XGI and XI.
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
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

// Formats a double the shortest way that round-trips ("1", "0.5"), as llama.cpp expects.
std::string format_number(double v);

// Cooperative cancellation (a portable stand-in for std::stop_source / std::stop_token,
// which older libc++ versions lack).
class StopToken {
public:
    StopToken() = default;  // never stops
    bool stop_requested() const;
    // Waits up to `seconds`. Returns true if a stop was requested.
    bool wait_for(double seconds) const;

private:
    friend class StopSource;
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        bool stopped = false;
    };
    explicit StopToken(std::shared_ptr<State> s) : state_(std::move(s)) {}
    std::shared_ptr<State> state_;
};

class StopSource {
public:
    StopSource();
    void request_stop();
    bool stop_requested() const { return token().stop_requested(); }
    bool wait_for(double seconds) const { return token().wait_for(seconds); }
    StopToken token() const { return StopToken(state_); }

private:
    std::shared_ptr<StopToken::State> state_;
};

// Sleeps for `seconds` or until stop is requested. Returns false if stopped.
inline bool interruptible_sleep(const StopToken& st, double seconds) { return !st.wait_for(seconds); }

}  // namespace xgi
