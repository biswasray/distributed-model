// Host facts reported by XI (architecture/02-node-agent.md §7).
// Implementations: sysinfo_windows.cpp, sysinfo_linux.cpp, sysinfo_macos.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace xgi::sysinfo {

std::uint64_t ram_total();
std::uint64_t ram_available();
int cpu_physical();
int cpu_logical();
std::string hostname();
// e.g. "Windows-11-10.0.26200-SP0", "Linux-6.8.0-45-generic-x86_64", "macOS-14.5-arm64"
std::string os_string();

}  // namespace xgi::sysinfo
