#include "common/net.hpp"

#include "common/platform.hpp"

#if defined(XGI_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
using socket_t = SOCKET;
constexpr socket_t kBadSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static bool set_nonblocking(socket_t s) {
    u_long nb = 1;
    return ioctlsocket(s, FIONBIO, &nb) == 0;
}
static bool in_progress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kBadSocket = -1;
static void close_socket(socket_t s) { ::close(s); }
static bool set_nonblocking(socket_t s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
static bool in_progress() { return errno == EINPROGRESS; }
#endif

namespace xgi {

void ensure_sockets() {
#if defined(XGI_WINDOWS)
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
}

namespace {

// Waits until a non-blocking connect finishes. True if it succeeded.
bool wait_connected(socket_t s, int timeout_ms) {
#if defined(XGI_WINDOWS)
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(s, &wfds);
    FD_SET(s, &efds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, nullptr, &wfds, &efds, &tv) > 0 && FD_ISSET(s, &wfds) && !FD_ISSET(s, &efds);
#else
    pollfd p{s, POLLOUT, 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return false;
    int err = 0;
    socklen_t len = sizeof(err);
    return getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
#endif
}

}  // namespace

bool tcp_probe(const std::string& host, int port, int timeout_ms) {
    ensure_sockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    bool ok = false;
    for (addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kBadSocket) continue;
        set_nonblocking(s);
        int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0) ok = true;
        else if (in_progress()) ok = wait_connected(s, timeout_ms);
        close_socket(s);
    }
    freeaddrinfo(res);
    return ok;
}

std::string route_ip_to(const std::string& host, int port) {
    ensure_sockets();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return {};
    std::string ip;
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != kBadSocket) {
        if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) {
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char buf[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                ip = buf;
            }
        }
        close_socket(s);
    }
    freeaddrinfo(res);
    if (ip == "0.0.0.0") ip.clear();
    return ip;
}

}  // namespace xgi
