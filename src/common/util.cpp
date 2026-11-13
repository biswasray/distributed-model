#include "common/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>

namespace xgi {

double wall_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

double mono_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::optional<std::string> get_env(const char* name) {
#if defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) return std::nullopt;
    std::string v(buf);
    free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

bool secure_equals(std::string_view a, std::string_view b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    return to_lower(std::string(s.substr(s.size() - suffix.size()))) == to_lower(std::string(suffix));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        out.emplace_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string format_number(double v) { return std::format("{}", v); }

bool StopToken::stop_requested() const {
    if (!state_) return false;
    std::lock_guard lk(state_->mu);
    return state_->stopped;
}

bool StopToken::wait_for(double seconds) const {
    if (!state_) {
        std::mutex mu;
        std::condition_variable cv;
        std::unique_lock lk(mu);
        cv.wait_for(lk, std::chrono::duration<double>(seconds), [] { return false; });
        return false;
    }
    std::unique_lock lk(state_->mu);
    return state_->cv.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return state_->stopped; });
}

StopSource::StopSource() : state_(std::make_shared<StopToken::State>()) {}

void StopSource::request_stop() {
    {
        std::lock_guard lk(state_->mu);
        state_->stopped = true;
    }
    state_->cv.notify_all();
}

}  // namespace xgi
