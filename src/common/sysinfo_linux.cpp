#include "common/sysinfo.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace xgi::sysinfo {

namespace {

// Reads a "Key:   12345 kB" line from /proc/meminfo, in bytes.
std::uint64_t meminfo(const std::string& key) {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key + ":", 0) != 0) continue;
        std::istringstream ss(line.substr(key.size() + 1));
        std::uint64_t kb = 0;
        ss >> kb;
        return kb * 1024;
    }
    return 0;
}

bool read_int(const std::string& path, int& out) {
    std::ifstream f(path);
    return static_cast<bool>(f >> out);
}

}  // namespace

std::uint64_t ram_total() { return meminfo("MemTotal"); }

std::uint64_t ram_available() {
    std::uint64_t v = meminfo("MemAvailable");
    return v ? v : meminfo("MemFree");
}

int cpu_logical() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : std::max(1u, std::thread::hardware_concurrency());
}

int cpu_physical() {
    // Unique (package, core) pairs from sysfs topology.
    std::set<std::pair<int, int>> cores;
    for (int cpu = 0; cpu < 4096; ++cpu) {
        std::string base = std::format("/sys/devices/system/cpu/cpu{}/topology/", cpu);
        int pkg = 0, core = 0;
        if (!read_int(base + "core_id", core)) {
            if (cpu > 0) break;
            continue;
        }
        read_int(base + "physical_package_id", pkg);
        cores.emplace(pkg, core);
    }
    return cores.empty() ? cpu_logical() : static_cast<int>(cores.size());
}

std::string hostname() {
    char buf[256]{};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "unknown-host";
}

std::string os_string() {
    utsname u{};
    if (uname(&u) != 0) return "Linux";
    return std::format("Linux-{}-{}", u.release, u.machine);
}

}  // namespace xgi::sysinfo
