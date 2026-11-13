#include "common/util.hpp"

#include <windows.h>

#include <algorithm>
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
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::nullopt;
    return narrow(std::wstring_view(buf, n));
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

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string format_number(double v) { return std::format("{}", v); }

bool interruptible_sleep(std::stop_token st, double seconds) {
    std::mutex mu;
    std::condition_variable_any cv;
    std::unique_lock lk(mu);
    cv.wait_for(lk, st, std::chrono::duration<double>(seconds), [] { return false; });
    return !st.stop_requested();
}

void StopFlag::set() {
    {
        std::lock_guard lk(mu_);
        set_ = true;
    }
    cv_.notify_all();
}

bool StopFlag::is_set() const {
    std::lock_guard lk(mu_);
    return set_;
}

bool StopFlag::wait_for(double seconds) {
    std::unique_lock lk(mu_);
    return cv_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return set_; });
}

}  // namespace xgi
