// mock-rpc-server: stands in for llama.cpp rpc-server in tests. Accepts rpc-server's flags
// (-H host -p port -t threads [-c]) and accepts TCP connections until killed.
// Always binds 127.0.0.1 so tests never trigger firewall prompts.
#include <httplib.h>  // pulls in the right socket headers for each OS

#include <iostream>
#include <string>
#include <thread>

#include "common/net.hpp"

#if defined(_WIN32)
using sock_t = SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static constexpr sock_t kBad = INVALID_SOCKET;
#else
using sock_t = int;
static void close_sock(sock_t s) { ::close(s); }
static constexpr sock_t kBad = -1;
#endif

int main(int argc, char** argv) {
    int port = 50052;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "-H") ++i;
        else if (a == "-p") port = std::stoi(argv[++i]);
    }
    xgi::ensure_sockets();
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if !defined(_WIN32)  // on Windows SO_REUSEADDR would let two listeners share the port
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        std::cout << "mock rpc-server: cannot listen on 127.0.0.1:" << port << std::endl;
        return 1;
    }
    std::cout << "mock rpc-server listening on 127.0.0.1:" << port << std::endl;
    while (true) {
        sock_t c = accept(s, nullptr, nullptr);
        if (c == kBad) break;
        std::thread([c] {
            char buf[4096];
            while (recv(c, buf, sizeof(buf), 0) > 0) {
            }
            close_sock(c);
        }).detach();
    }
    return 0;
}
