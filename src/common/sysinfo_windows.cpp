#include "common/sysinfo.hpp"

#include <windows.h>

#include <format>
#include <vector>

namespace xgi::sysinfo {

namespace {
std::string to_utf8(const wchar_t* w, int n) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, out.data(), len, nullptr, nullptr);
    return out;
}
}  // namespace

std::uint64_t ram_total() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

std::uint64_t ram_available() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
}

int cpu_logical() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? static_cast<int>(n) : 1;
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

std::string hostname() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 64];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return to_utf8(buf, static_cast<int>(n));
    n = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameW(buf, &n)) return to_utf8(buf, static_cast<int>(n));
    return "unknown-host";
}

std::string os_string() {
    // RtlGetVersion is not subject to the manifest-based version lie of GetVersionEx.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))))
            fn(&v);
    }
    std::string release = std::to_string(v.dwMajorVersion);
    if (v.dwMajorVersion == 10) release = v.dwBuildNumber >= 22000 ? "11" : "10";
    return std::format("Windows-{}-{}.{}.{}-SP0", release, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

}  // namespace xgi::sysinfo
