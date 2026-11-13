#include "common/sysinfo.hpp"

#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <format>
#include <string>

namespace xgi::sysinfo {

namespace {

template <class T>
bool sysctl_value(const char* name, T& out) {
    size_t len = sizeof(T);
    return sysctlbyname(name, &out, &len, nullptr, 0) == 0;
}

std::string sysctl_string(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string s(len, '\0');
    if (sysctlbyname(name, s.data(), &len, nullptr, 0) != 0) return {};
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

}  // namespace

std::uint64_t ram_total() {
    std::uint64_t v = 0;
    return sysctl_value("hw.memsize", v) ? v : 0;
}

std::uint64_t ram_available() {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS)
        return 0;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    // Free + inactive + purgeable pages are reclaimable without swapping.
    return (static_cast<std::uint64_t>(vm.free_count) + vm.inactive_count + vm.purgeable_count) * page;
}

int cpu_logical() {
    int v = 0;
    return sysctl_value("hw.logicalcpu", v) && v > 0 ? v : 1;
}

int cpu_physical() {
    int v = 0;
    return sysctl_value("hw.physicalcpu", v) && v > 0 ? v : cpu_logical();
}

std::string hostname() {
    char buf[256]{};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "unknown-host";
}

std::string os_string() {
    utsname u{};
    uname(&u);
    std::string ver = sysctl_string("kern.osproductversion");
    return std::format("macOS-{}-{}", ver.empty() ? u.release : ver, u.machine);
}

}  // namespace xgi::sysinfo
