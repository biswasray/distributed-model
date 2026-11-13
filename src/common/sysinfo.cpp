#include "common/sysinfo.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <format>
#include <vector>

#include "common/http_util.hpp"
#include "common/util.hpp"

namespace xgi::sysinfo {

std::uint64_t ram_total() {
    MEMORYSTATUSEX m{sizeof(m)};
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

std::uint64_t ram_available() {
    MEMORYSTATUSEX m{sizeof(m)};
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
}

int cpu_physical() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return cpu_logical();
    std::vector<char> buf(len);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) return cpu_logical();
    int cores = 0;
    for (DWORD off = 0; off < len;) {
        auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (p->Relationship == RelationProcessorCore) ++cores;
        off += p->Size;
    }
    return cores > 0 ? cores : cpu_logical();
}

int cpu_logical() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? static_cast<int>(n) : 1;
}

std::string hostname() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 64];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return narrow(std::wstring_view(buf, n));
    n = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameW(buf, &n)) return narrow(std::wstring_view(buf, n));
    return "unknown-host";
}

std::string os_string() {
    // RtlGetVersion is not subject to the manifest-based version lie of GetVersionEx.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{sizeof(v)};
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&v);
    }
    std::string release = std::to_string(v.dwMajorVersion);
    if (v.dwMajorVersion == 10) release = v.dwBuildNumber >= 22000 ? "11" : "10";
    return std::format("Windows-{}-{}.{}.{}-SP0", release, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

std::string route_ip_to(const std::string& host, int port) {
    ensure_winsock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return {};
    std::string ip;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        if (connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) {
            sockaddr_in local{};
            int len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char buf[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                ip = buf;
            }
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    if (ip == "0.0.0.0") ip.clear();
    return ip;
}

}  // namespace xgi::sysinfo
