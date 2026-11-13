# ADR-008: Cross-platform XGI/XI (Windows, Linux, macOS) with any C++20 compiler

- **Status:** Accepted
- **Date:** 2026-11-13
- **Amends:** [ADR-007](ADR-007-cpp-implementation-xgi-xi.md), whose decision named MSVC as the toolchain and Windows as the only target

## Context

ADR-007 built XGI and XI with MSVC for Windows only. Operators want to:

- run the control machine (XGI) on whatever box is available, often a Linux server or a Mac;
- add Linux PCs to the cluster as nodes;
- build without Visual Studio, with GCC, Clang or MinGW-w64, in CI or by cross-compiling.

llama.cpp already ships native builds for all three OSes, so nothing in the inference path is Windows-only. The original invariant, "Windows-native, nothing Linux-only", was about never *requiring* Linux. It doesn't forbid supporting it.

## Decision

1. **Targets:** Windows 10/11 (x64), Linux (x86-64, arm64), and macOS 13.3+ (arm64, x86-64). Windows stays the primary node platform, so every feature must work there.
2. **Toolchains:** any C++20 compiler with `<format>`: MSVC 19.30+ (VS 2022+), GCC 13+, Clang 17+ with libstdc++ or libc++, Apple Clang 15+ (Xcode 15+), and MinGW-w64 GCC 13+ (native or cross-compiled, toolchain file in `cmake/toolchains/`). The CMake build has no compiler-specific requirements, only per-compiler warning flags.
3. **OS code is isolated** behind small interfaces, with one implementation file per OS:

   | Concern | Windows | POSIX |
   |---|---|---|
   | Child processes (`src/xi/process_*.cpp`) | `CreateProcessW`, new process group, kill-on-close job object | `fork`/`execvp`, new process group (`setpgid`), `SIGTERM` → `SIGKILL` to the group, Linux `PR_SET_PDEATHSIG` |
   | Host facts (`src/common/sysinfo_*.cpp`) | Win32 APIs | Linux `/proc` and `/sys`; macOS `sysctl` and Mach VM statistics |
   | Shutdown (`src/common/shutdown.cpp`) | Console control handler | `SIGINT`/`SIGTERM` through a self-pipe; `SIGPIPE` ignored |
   | Sockets (`src/common/net.cpp`) | Winsock | BSD sockets |

4. **Portable C++ only.** No `std::jthread`, `std::stop_token` or `<chrono>` time zones, because older libc++ versions (Apple) lack them. Small replacements live in `common/util.hpp` and `common/log.cpp`.
5. **Wire contracts are unchanged** ([05](../architecture/05-protocols-and-data.md)). A node's `os` field reports e.g. `Linux-6.8.0-45-generic-x86_64` or `macOS-14.5-arm64`.
6. **Tests:** unit tests run everywhere. The full e2e test (`run-e2e.ps1`) runs under Windows PowerShell 5.1 or PowerShell 7 on any OS. A `sh` + `curl` smoke test (`smoke.sh`) covers Linux and macOS without PowerShell.

## Alternatives considered

| Option | Why not |
|---|---|
| Stay Windows/MSVC-only | Blocks Linux/macOS control machines, CI on Linux runners, and builds without Visual Studio |
| Boost (Asio/Process) for portability | Heavy dependency for a few hundred lines of OS code |
| Separate codebases per OS | Duplicated logic, and the platforms drift apart |

## Consequences

- ✅ One codebase builds with MSVC, MinGW-w64, GCC and Clang. Verified 2026-11-13: MSVC 14.51 and MinGW-w64 GCC 13 (Windows 11), plus GCC 13.3 and Clang 18 with libc++ (Ubuntu 24.04). Unit and end-to-end tests pass on each.
- ✅ The control machine and nodes can run any of the three OSes.
- ⚠️ **macOS is supported but not yet verified on hardware.** The code compiles against the documented APIs only.
- ❌ macOS has no parent-death signal. If XI crashes there, its llama.cpp child keeps running until it is stopped (`launchd` restarts XI, and the next `/rpc/start` or `/head/start` replaces it). On Linux and Windows, children die with XI.
- ❌ Mixed-OS groups (for example a Windows head with Linux workers) depend on llama.cpp's RPC staying compatible across platform builds of the **same release**. This is untested. Prefer one OS per group.
- 🔜 CI matrix (Windows MSVC, Ubuntu GCC/Clang, macOS Apple Clang).
