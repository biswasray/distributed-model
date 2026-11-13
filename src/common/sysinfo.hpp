// Host facts reported by XI (architecture/02-node-agent.md §7).
#pragma once

#include <cstdint>
#include <string>

namespace xgi::sysinfo {

std::uint64_t ram_total();
std::uint64_t ram_available();
int cpu_physical();
int cpu_logical();
std::string hostname();
// e.g. "Windows-11-10.0.26200-SP0"
std::string os_string();
// The local IP used to reach host:port (UDP "connect" trick; nothing is sent). Empty if unknown.
std::string route_ip_to(const std::string& host, int port);

}  // namespace xgi::sysinfo
