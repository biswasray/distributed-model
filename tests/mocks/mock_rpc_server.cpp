// mock-rpc-server.exe: stands in for llama.cpp rpc-server in tests. Accepts rpc-server's flags
// (-H host -p port -t threads [-c]) and accepts TCP connections until killed.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 50052;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "-H") ++i;  // always 127.0.0.1: tests must not trigger firewall prompts
        else if (a == "-p") port = std::stoi(argv[++i]);
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        std::cout << "mock rpc-server: cannot listen on " << host << ":" << port << std::endl;
        return 1;
    }
    std::cout << "mock rpc-server listening on " << host << ":" << port << std::endl;
    while (true) {
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::thread([c] {
            char buf[4096];
            while (recv(c, buf, sizeof(buf), 0) > 0) {
            }
            closesocket(c);
        }).detach();
    }
    return 0;
}
