// Platform detection and small portability helpers (ADR-008).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define XGI_WINDOWS 1
#elif defined(__APPLE__)
#define XGI_MACOS 1
#define XGI_POSIX 1
#elif defined(__linux__)
#define XGI_LINUX 1
#define XGI_POSIX 1
#else
#define XGI_POSIX 1  // other Unix: best effort
#endif

namespace xgi {

#if defined(XGI_WINDOWS)
inline constexpr std::string_view kExeSuffix = ".exe";
inline constexpr std::string_view kPlatformName = "windows";
#elif defined(XGI_MACOS)
inline constexpr std::string_view kExeSuffix = "";
inline constexpr std::string_view kPlatformName = "macos";
#else
inline constexpr std::string_view kExeSuffix = "";
inline constexpr std::string_view kPlatformName = "linux";
#endif

// CPU architecture this binary was built for.
#if defined(_M_X64) || defined(__x86_64__)
inline constexpr std::string_view kArchName = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
inline constexpr std::string_view kArchName = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
inline constexpr std::string_view kArchName = "x86";
#else
inline constexpr std::string_view kArchName = "unknown";
#endif

// UTF-8 <-> std::filesystem::path, the same way on every OS.
inline std::filesystem::path path_from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
inline std::string path_to_utf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
inline std::string generic_utf8(const std::filesystem::path& p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace xgi
