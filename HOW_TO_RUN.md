# How to run XGI and XI

A hands-on guide to building, testing, and running the Qwen Cluster control plane on **Windows, Linux, or macOS**:

- **XGI** (`xgi` / `xgi.exe`): the coordinator (`:8100`) and the router (`:8000`)
- **XI** (`xi` / `xi.exe`): the node agent (`:8200`), which runs on every PC

Inference itself is done by llama.cpp (`llama-server`, `rpc-server`). For the design, see the [docs index](README.md). For the full operator view of the Windows fleet, see the [operator guide](distributed-qwen-cluster.md). Platform support is described in [ADR-008](adr/ADR-008-cross-platform-build.md).

| Step | What you get | Time |
|---|---|---|
| [1. Build](#1-build) | `xgi`, `xi`, test binaries | ~5 min (first build) |
| [2. Test](#2-run-the-tests) | Unit and end-to-end tests with mock llama.cpp | ~1–2 min |
| [3. Try it on one machine](#3-try-it-on-one-machine-with-a-real-model) | A real Qwen 7B behind the router, on your own machine | ~15 min |
| [4. Run the cluster](#4-run-the-real-cluster) | The full multi-PC deployment | per node |
| [5. Operate it](#5-day-to-day-operations) | Status, drain, restart, logs | — |

---

## 1. Build

### Supported platforms and compilers

| OS | Compilers | Verified |
|---|---|---|
| Windows 10/11 x64 | MSVC 19.30+ (VS 2022/2026), MinGW-w64 GCC 13+, clang-cl / Clang 17+ | MSVC 14.51, MinGW-w64 GCC 13 |
| Linux x86-64 / arm64 | GCC 13+, Clang 17+ (libstdc++ or libc++) | GCC 13.3, Clang 18 + libc++ (Ubuntu 24.04) |
| macOS 13.3+ (arm64 / x86-64) | Apple Clang 15+ (Xcode 15+), Homebrew LLVM 17+ | Not yet verified on hardware |

You need **CMake 3.25+**, **Ninja** (recommended), and internet access for the first configure. CMake downloads cpp-httplib, nlohmann/json, and yaml-cpp, and checks each against a pinned SHA-256 (see `cmake/Dependencies.cmake`).

### Install the tools

| OS | Command |
|---|---|
| Ubuntu / Debian | `sudo apt install build-essential cmake ninja-build curl` (Ubuntu 24.04+ for GCC 13) |
| Fedora | `sudo dnf install gcc-c++ cmake ninja-build curl` |
| macOS | `xcode-select --install`, then `brew install cmake ninja` |
| Windows (MSVC) | Visual Studio 2022+ with **Desktop development with C++** (it includes CMake and Ninja) |
| Windows (MinGW) | e.g. MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja` |

### Build: Linux, macOS, or any compiler on PATH

```sh
cd distributed-model
cmake --preset release            # Ninja, Release; uses $CXX or the default c++ compiler
cmake --build --preset release
```

To choose a compiler: `CXX=clang++ cmake --preset release`, or `-DCMAKE_CXX_COMPILER=g++-13`. With libc++: add `-DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++`.

Without Ninja: `cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release -j`.

### Build: Windows with Visual Studio

Run this in any PowerShell. The VS generator doesn't need a Developer prompt:

```powershell
# Only if cmake isn't on PATH (adjust "18" to your VS version):
$env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"

cmake --preset vs                 # VS 2026 (needs CMake 4.2+); use --preset vs2022 for Visual Studio 2022
cmake --build --preset vs-release # or vs2022-release
```

From a **Developer PowerShell for VS**, or an MSYS2/MinGW shell, the Ninja presets above (`--preset release`) work on Windows too.

### Cross-compile Windows binaries from Linux or macOS (MinGW-w64)

```sh
sudo apt install mingw-w64          # Debian/Ubuntu; macOS: brew install mingw-w64
cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
cmake --build build/mingw           # -> build/mingw/xgi.exe, xi.exe (static, no DLLs needed)
```

### Output

| Preset | Executables | Tests and mocks |
|---|---|---|
| `release` (Ninja) | `build/release/xgi`, `build/release/xi` (`.exe` on Windows) | `build/release/tests/` |
| `vs` | `build\vs\Release\xgi.exe`, `xi.exe` | `build\vs\tests\Release\` |

The C++ runtime is linked statically where the toolchain allows it (MSVC `/MT`, GCC `-static-libstdc++`, MinGW `-static`), so nodes don't need a runtime installed. Turn this off with `-DXGI_STATIC_RUNTIME=OFF`.

> **Offline build:** download the three archives listed in `cmake/Dependencies.cmake`, extract them, and pass `-DFETCHCONTENT_SOURCE_DIR_HTTPLIB=…`, `-DFETCHCONTENT_SOURCE_DIR_JSON=…`, and `-DFETCHCONTENT_SOURCE_DIR_YAML-CPP=…`.

> **Windows "An Application Control policy has blocked this file":** Smart App Control or WDAC blocks unsigned executables it hasn't checked yet. Turn off Smart App Control on the build machine, or sign or allow the binaries. **macOS Gatekeeper:** binaries you build locally run as-is. If you copy them to another Mac, clear the quarantine flag with `xattr -d com.apple.quarantine xi xgi`.

### Package for deployment

```sh
cmake --install build/release --prefix /opt/qwen-cluster               # Linux / macOS
```
```powershell
cmake --install build\vs --config Release --prefix C:\qwen-cluster      # Windows
```

This creates `bin/xgi`, `bin/xi`, `cluster.yaml` (an example config), and `scripts/` (`install-xi-task.ps1` for Windows, `install-xi-service.sh` for systemd or launchd).

---

## 2. Run the tests

```sh
ctest --preset release            # Ninja builds
```
```powershell
ctest --preset vs-release         # Visual Studio builds
```

| Test | Where | What it does |
|---|---|---|
| `unit` | everywhere | Config validation, placement algorithm (including a 50-node hybrid layout), router queueing and least-busy selection, llama.cpp command lines, model-path traversal guard |
| `e2e` | Windows, or any OS with PowerShell 7 (`pwsh`) | `xgi serve` + 4 `xi` agents on `127.0.0.1` with mock llama.cpp: auth, deploy, routing, streaming, worker failure → repair onto the spare, head crash → restart, drain/undrain, restart, undeploy (37 checks) |
| `smoke` | Linux / macOS (needs `curl`) | `sh` version of the core of the e2e test: register, deploy, routing, streaming, worker failure → repair, head crash → restart, undeploy |

Run them by hand and keep the logs:

```sh
B=build/release
sh tests/e2e/smoke.sh $B/xgi $B/xi $B/tests/mock-llama-server $B/tests/mock-rpc-server
pwsh tests/e2e/run-e2e.ps1 -Xgi $B/xgi -Xi $B/xi -MockLlama $B/tests/mock-llama-server \
     -MockRpc $B/tests/mock-rpc-server -WorkDir /tmp/xgi-e2e -KeepWorkDir
```
```powershell
$b = "build\vs"
powershell -NoProfile -ExecutionPolicy Bypass -File tests\e2e\run-e2e.ps1 `
  -Xgi "$b\Release\xgi.exe" -Xi "$b\Release\xi.exe" `
  -MockLlama "$b\tests\Release\mock-llama-server.exe" -MockRpc "$b\tests\Release\mock-rpc-server.exe" `
  -WorkDir C:\temp\xgi-e2e -KeepWorkDir
```

The logs are in `<WorkDir>/logs-xgi/xgi.log` (coordinator and router) and `logs-<node>/` (each agent's `xi.log`, `head.log`, and `rpc.log`).

---

## 3. Try it on one machine with a real model

This runs the whole stack on one machine: XGI, one XI agent, and a single-node `qwen-7b` pool.

### 3.1 Get llama.cpp and a model

1. Get llama.cpp with `llama-server` and `rpc-server`:
   - **Windows:** download a **CPU (AVX2)** release from the official llama.cpp GitHub releases, and extract it to `C:\qwen-cluster\llama.cpp\`.
   - **Linux / macOS:** use the official release for your OS, or build it: `cmake -B build -DGGML_RPC=ON && cmake --build build -j`. Put `llama-server` and `rpc-server` in `/opt/qwen-cluster/llama.cpp/`. On macOS, Metal is enabled by default; the cluster design assumes CPU, so it works either way.
2. Download `qwen2.5-7b-instruct-q4_k_m.gguf` (~4.7 GB) from `Qwen/Qwen2.5-7B-Instruct-GGUF` on Hugging Face, and put it in the `models` folder next to `llama.cpp`. If the download comes split into parts, use the first part's file name in `model_file`.

### 3.2 Write a one-pool config

Save this as `local.yaml` in the install folder (`C:\qwen-cluster` or `/opt/qwen-cluster`):

```yaml
cluster_token: ""            # set QWEN_CLUSTER_TOKEN instead (next step)

coordinator:
  host: 127.0.0.1
  port: 8100

router:
  host: 127.0.0.1
  port: 8000
  coordinator_url: http://127.0.0.1:8100
  api_keys: []

pools:
  - name: qwen-7b
    model_file: qwen2.5-7b-instruct-q4_k_m.gguf
    nodes_per_group: 1
    groups: 1
    context: 4096

spares: 0
```

Check it with `bin/xgi check-config --config local.yaml` (Windows: `.\bin\xgi.exe check-config --config local.yaml`).

### 3.3 Start XGI and XI (two terminals)

**Linux / macOS**

```sh
# terminal 1
cd /opt/qwen-cluster && export QWEN_CLUSTER_TOKEN=local-test-token
bin/xgi serve --config local.yaml

# terminal 2
cd /opt/qwen-cluster && export QWEN_CLUSTER_TOKEN=local-test-token
bin/xi --coordinator http://127.0.0.1:8100 --host 127.0.0.1 --advertise-ip 127.0.0.1 \
       --llama-dir llama.cpp --models-dir models --log-dir logs
```

**Windows**

```powershell
# terminal 1
cd C:\qwen-cluster; $env:QWEN_CLUSTER_TOKEN = "local-test-token"
.\bin\xgi.exe serve --config local.yaml

# terminal 2
cd C:\qwen-cluster; $env:QWEN_CLUSTER_TOKEN = "local-test-token"
.\bin\xi.exe --coordinator http://127.0.0.1:8100 --host 127.0.0.1 --advertise-ip 127.0.0.1 `
  --llama-dir C:\qwen-cluster\llama.cpp --models-dir C:\qwen-cluster\models --log-dir C:\qwen-cluster\logs
```

Within about 5 seconds, terminal 1 logs `node <hostname> registered`.

### 3.4 Deploy and ask a question

**Linux / macOS (curl)**

```sh
T="X-Cluster-Token: $QWEN_CLUSTER_TOKEN"
curl -s -H "$T" http://127.0.0.1:8100/nodes
curl -s -X POST -H "$T" http://127.0.0.1:8100/deploy
curl -s -H "$T" http://127.0.0.1:8100/groups        # repeat until "status":"healthy"

curl -s http://127.0.0.1:8000/v1/chat/completions -H "Content-Type: application/json" \
  -d '{"model":"qwen-7b","max_tokens":200,"messages":[{"role":"user","content":"Explain pipeline parallelism in 2 sentences."}]}'

curl -N http://127.0.0.1:8000/v1/chat/completions -H "Content-Type: application/json" \
  -d '{"model":"qwen-7b","stream":true,"messages":[{"role":"user","content":"Hello!"}]}'
```

**Windows (PowerShell)**

```powershell
$T = @{ "X-Cluster-Token" = $env:QWEN_CLUSTER_TOKEN }
Invoke-RestMethod http://127.0.0.1:8100/nodes -Headers $T | Format-Table node_id, status, models
Invoke-RestMethod -Method Post http://127.0.0.1:8100/deploy -Headers $T
Invoke-RestMethod http://127.0.0.1:8100/groups -Headers $T | Format-Table group_id, status, head_url, error

$body = @{ model = "qwen-7b"; max_tokens = 200
           messages = @(@{ role = "user"; content = "Explain pipeline parallelism in 2 sentences." }) } | ConvertTo-Json -Depth 5
(Invoke-RestMethod -Method Post http://127.0.0.1:8000/v1/chat/completions -ContentType "application/json" -Body $body).choices[0].message.content

curl.exe -N http://127.0.0.1:8000/v1/chat/completions -H "Content-Type: application/json" `
  -d '{\"model\":\"qwen-7b\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"Hello!\"}]}'
```

Any OpenAI SDK works too: set `base_url="http://127.0.0.1:8000/v1"` and any `api_key`.

Press **Ctrl+C** in terminal 2 to stop XI. It stops `llama-server` first (Ctrl+C or `SIGTERM`).

### 3.5 Demo without a model (mock binaries)

Point XI at the mocks. Any file ending in `.gguf` in the models folder is enough:

```sh
mkdir -p /tmp/mock-models && echo GGUF > /tmp/mock-models/qwen2.5-7b-instruct-q4_k_m.gguf
bin/xi --coordinator http://127.0.0.1:8100 --host 127.0.0.1 --advertise-ip 127.0.0.1 \
       --models-dir /tmp/mock-models \
       --llama-server-bin /path/to/build/release/tests/mock-llama-server \
       --rpc-server-bin  /path/to/build/release/tests/mock-rpc-server
```

On Windows, use the same flags with the `.exe` mocks from `build\vs\tests\Release\`. The mock answers with `Hello from mock llama-server …` and streams fake tokens.

---

## 4. Run the real cluster

The short version is below. The full procedure for the Windows fleet, including firewall rules, static IPs, and power settings, is in the [operator guide §7](distributed-qwen-cluster.md#7-installation) and the `node-setup-runbook` skill.

> **Keep each group on one OS.** Every node must run the **same llama.cpp release**. Mixing Windows and Linux builds inside one group depends on RPC compatibility across platform builds, which is untested.

### 4.1 Every node

**Windows** (elevated PowerShell):

```powershell
# Files: C:\qwen-cluster\bin\xi.exe, scripts\install-xi-task.ps1, llama.cpp\ ; heads also need models\<model_file>
New-NetFirewallRule -DisplayName "Qwen RPC"   -Direction Inbound -Protocol TCP -LocalPort 50052 -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24
New-NetFirewallRule -DisplayName "Qwen Agent" -Direction Inbound -Protocol TCP -LocalPort 8200  -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24
New-NetFirewallRule -DisplayName "Qwen Head"  -Direction Inbound -Protocol TCP -LocalPort 8080  -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24

[Environment]::SetEnvironmentVariable("QWEN_CLUSTER_TOKEN", "<long random string>", "Machine")
C:\qwen-cluster\scripts\install-xi-task.ps1 -Coordinator http://192.168.1.10:8100
Start-ScheduledTask -TaskName QwenNodeAgent
```

**Linux** (as root):

```sh
# Files: /opt/qwen-cluster/bin/xi, scripts/install-xi-service.sh, llama.cpp/ ; heads also need models/<model_file>
ufw allow from 192.168.1.0/24 to any port 50052,8200,8080 proto tcp     # or the firewalld equivalent

echo 'QWEN_CLUSTER_TOKEN=<long random string>' > /etc/qwen-cluster.env && chmod 600 /etc/qwen-cluster.env
/opt/qwen-cluster/scripts/install-xi-service.sh http://192.168.1.10:8100     # systemd unit "qwen-xi"
journalctl -u qwen-xi -f
```

**macOS** (with sudo): the same `install-xi-service.sh` installs a launchd daemon (`org.qwen-cluster.xi`). Allow incoming connections for `xi`, `rpc-server`, and `llama-server` in the macOS firewall, and turn off sleep (`sudo pmset -a sleep 0`).

XI finds its own IP from the route to the coordinator. Use `--advertise-ip` only on multi-homed machines. Run `xi --help` to see every flag.

### 4.2 Control machine (wired, if possible)

```sh
cd /opt/qwen-cluster                        # Windows: cd C:\qwen-cluster and use .\bin\xgi.exe
$EDITOR cluster.yaml                        # pools, groups, spares (schema: architecture/05 §5)
bin/xgi check-config --config cluster.yaml  # validate + node count needed
bin/xgi serve --config cluster.yaml         # or run "coordinator" and "router" separately
```

Open port 8000 to clients and port 8100 to the cluster subnet. On Windows:

```powershell
New-NetFirewallRule -DisplayName "XGI Router"      -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "XGI Coordinator" -Direction Inbound -Protocol TCP -LocalPort 8100 -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24
```

To keep XGI running across reboots on Linux, a systemd unit like the agent's works: `ExecStart=/opt/qwen-cluster/bin/xgi serve --config /opt/qwen-cluster/cluster.yaml`, with `EnvironmentFile=/etc/qwen-cluster.env`.

### 4.3 Deploy

```sh
T="X-Cluster-Token: $QWEN_CLUSTER_TOKEN"; C=http://192.168.1.10:8100
curl -s -H "$T" $C/nodes             # all nodes alive?
curl -s -X POST -H "$T" $C/deploy
curl -s -H "$T" $C/groups
```
```powershell
$T = @{ "X-Cluster-Token" = $env:QWEN_CLUSTER_TOKEN }; $C = "http://192.168.1.10:8100"
Invoke-RestMethod "$C/nodes" -Headers $T | Format-Table node_id, ip, status, os, models
Invoke-RestMethod -Method Post "$C/deploy" -Headers $T
Invoke-RestMethod "$C/groups" -Headers $T | Format-Table group_id, status, head, workers, error
```

> ⏱ **The first deployment is slow.** Workers receive their weight slices over Wi-Fi, which can take 30–60+ minutes for 72B. Groups stay `loading` meanwhile. Later deploys load from each worker's `rpc-server -c` cache.

Clients use `http://192.168.1.10:8000/v1`. If `router.api_keys` is set, they also send `Authorization: Bearer <key>`.

---

## 5. Day-to-day operations

All coordinator calls except `/health` need `X-Cluster-Token`. The table uses curl, with `$T` and `$C` set as in §4.3. On Windows, use `curl.exe` or the `Invoke-RestMethod` forms above.

| Task | Command |
|---|---|
| Cluster status | `curl -s -H "$T" $C/groups` |
| Node list | `curl -s -H "$T" $C/nodes` |
| Router load per pool | `curl -s http://192.168.1.10:8000/health` |
| Take a PC out before maintenance | `curl -s -X POST -H "$T" $C/drain/LAB-PC-07` |
| Put it back | `curl -s -X POST -H "$T" $C/undrain/LAB-PC-07` |
| Restart one group | `curl -s -X POST -H "$T" $C/groups/qwen3-30b-a3b-3/restart` |
| Apply a changed `cluster.yaml` | Restart `xgi`, then `curl -s -X POST -H "$T" "$C/deploy?force=true"` |
| Stop everything | `curl -s -X POST -H "$T" $C/undeploy` |
| Last 100 lines of a head's llama-server | `curl -s -H "$T" "http://<node-ip>:8200/logs/head?lines=100"` |

### What happens on its own

- **A node stops sending heartbeats for 30 s.** It is marked `dead`, and its group becomes `degraded`. The coordinator stops the survivors, swaps in a spare, and restarts the group.
- **A llama.cpp process exits, or `/health` fails 3 times.** Same repair. Repeated failures back off, up to 5 minutes between attempts.
- **The coordinator restarts.** Agents re-register within about 5 s. Heads keep serving, and the router uses its last known table. Run `POST /deploy` again to restore management ([ADR-006](adr/ADR-006-in-memory-coordinator-state.md)).

### Log files

| Machine | File |
|---|---|
| Control | `logs/xgi.log` (`serve`), or `logs/xgi-coordinator.log` and `logs/xgi-router.log` |
| Node | `logs/xi.log` (agent), `logs/head.log` (llama-server), `logs/rpc.log` (rpc-server). Under systemd, also `journalctl -u qwen-xi`. |

Use `--log-level debug` on either executable for more detail.

### Quick troubleshooting

| Symptom | Check |
|---|---|
| Node missing from `/nodes` | Is the agent running (`Get-ScheduledTask QwenNodeAgent`, `systemctl status qwen-xi`, `launchctl print system/org.qwen-cluster.xi`)? Look for `401` or `register ... failed` in `logs/xi.log`, which usually means a token mismatch or the wrong coordinator URL. |
| Group `unplaced` | Its `error` field says why: no free node has the model file, would consume spares, or not enough nodes. |
| Group `failed` with `rpc-server ... did not open` | Firewall on port 50052, or the wrong `rpc-server` path. Test with `Test-NetConnection <ip> -Port 50052` or `nc -zv <ip> 50052`. |
| `head/start ... cannot start` | `llama-server` isn't in `--llama-dir` (no `.exe` suffix on Linux/macOS), or it isn't executable (`chmod +x`). |
| Group stuck in `loading` | Expected on the first deploy. Watch `GET /logs/head` on the head node. |
| Router `503 model unavailable` | No `healthy` group in that pool, or a misspelled `model`. |
| Router `429 queue full` | All groups busy and the pool queue is at `max_queue`. Add groups or retry after `Retry-After`. |

For deeper diagnosis, use the `cluster-troubleshooting` skill or the [operator guide §14](distributed-qwen-cluster.md#14-troubleshooting).

> Performance figures in these docs (tokens/s per pool) are **estimates** until benchmarked on your hardware.
