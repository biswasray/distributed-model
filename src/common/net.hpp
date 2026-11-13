// Portable socket helpers (Winsock on Windows, BSD sockets elsewhere).
#pragma once

#include <string>

namespace xgi {

// Calls WSAStartup once on Windows; no-op elsewhere. Safe to call repeatedly.
void ensure_sockets();

// True if a TCP connection to host:port succeeds within timeout_ms.
bool tcp_probe(const std::string& host, int port, int timeout_ms);

// The local IPv4 address used to reach host:port (UDP "connect" trick; nothing is sent). Empty if unknown.
std::string route_ip_to(const std::string& host, int port);

}  // namespace xgi
