# How to run XGI and XI

A hands-on guide to building, testing, and running the Qwen Cluster control plane:

- **XGI** (`xgi.exe`): the coordinator (`:8100`) and the router (`:8000`)
- **XI** (`xi.exe`): the node agent (`:8200`), which runs on every PC

Inference itself is done by llama.cpp (`llama-server`, `rpc-server`). For the design, see the [docs index](README.md). For the full operator view, see the [operator guide](distributed-qwen-cluster.md).

| Step | What you get | Time |
|---|---|---|
| [1. Build](#1-build) | `xgi.exe`, `xi.exe`, test binaries | ~5 min (first build) |
| [2. Test](#2-run-the-tests) | Unit and end-to-end tests with mock llama.cpp | ~1 min |
| [3. Try it on one PC](#3-try-it-on-one-pc-with-a-real-model) | A real Qwen 7B behind the router, on your own machine | ~15 min |
| [4. Run the cluster](#4-run-the-real-cluster) | The full multi-PC deployment | per node |
| [5. Operate it](#5-day-to-day-operations) | Status, drain, restart, logs | — |

---

## 1. Build

### Requirements

- Windows 10/11 x64
- Visual Studio 2022 or later (Community is fine), with the **Desktop development with C++** workload. It includes MSVC, CMake, and Ninja.
- Internet access for the first configure. CMake downloads cpp-httplib, nlohmann/json, and yaml-cpp, and checks each against a pinned SHA-256 (see `cmake/Dependencies.cmake`).

### Commands

Run these in a **Developer PowerShell for VS**, or in a normal PowerShell after putting the bundled CMake on `PATH`:

```powershell
# Only needed outside a Developer PowerShell (adjust "18" to your VS version):
$env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"

cd C:\path\to\distributed-model
cmake --preset vs                 # configure (Visual Studio generator, x64)
cmake --build --preset release    # build Release
```

The output goes to:

| File | Purpose |
|---|---|
| `build\vs\Release\xgi.exe` | Coordinator and router |
| `build\vs\Release\xi.exe` | Node agent |
| `build\vs\tests\Release\xgi_tests.exe` | Unit tests |
| `build\vs\tests\Release\mock-llama-server.exe`, `mock-rpc-server.exe` | Fake llama.cpp binaries for tests and demos |

> **Offline build:** download the three archives listed in `cmake/Dependencies.cmake`, extract them, and pass `-DFETCHCONTENT_SOURCE_DIR_HTTPLIB=...`, `-DFETCHCONTENT_SOURCE_DIR_JSON=...`, and `-DFETCHCONTENT_SOURCE_DIR_YAML-CPP=...` to `cmake --preset vs`.

> **"An Application Control policy has blocked this file":** Windows Smart App Control or WDAC blocks unsigned executables it hasn't checked yet. Turn off Smart App Control on the build machine, or sign the binaries or allow them through your policy. Nodes need the same treatment.

### Package for deployment

```powershell
cmake --install build\vs --config Release --prefix C:\qwen-cluster
```

This creates:

```
C:\qwen-cluster\
├── bin\xgi.exe
├── bin\xi.exe
├── cluster.yaml                 ← example config (operator guide option D)
└── scripts\install-xi-task.ps1  ← registers xi.exe as a startup task
```

Both executables link the C++ runtime statically, so nodes don't need a VC++ redistributable.

---

## 2. Run the tests

```powershell
ctest --preset release
```

| Test | What it does |
|---|---|
| `unit` | Config validation, placement algorithm (including a 50-node hybrid layout), router queueing and least-busy selection, llama.cpp command lines, model-path traversal guard |
| `e2e` | Starts `xgi serve` and 4 `xi` agents on `127.0.0.1` with the mock llama.cpp binaries, then checks auth, deploy, routing, streaming, worker failure → repair onto the spare, head crash → restart, drain/undrain, restart, and undeploy |

To run the end-to-end test by hand and keep its logs:

```powershell
$b = "build\vs"
powershell -NoProfile -ExecutionPolicy Bypass -File tests\e2e\run-e2e.ps1 `
  -Xgi "$b\Release\xgi.exe" -Xi "$b\Release\xi.exe" `
  -MockLlama "$b\tests\Release\mock-llama-server.exe" -MockRpc "$b\tests\Release\mock-rpc-server.exe" `
  -WorkDir C:\temp\xgi-e2e -KeepWorkDir
```

The logs are in `C:\temp\xgi-e2e\logs-xgi\xgi.log` (coordinator and router) and `logs-<node>\` (each agent's `xi.log`, `head.log`, and `rpc.log`).

---

## 3. Try it on one PC with a real model

This runs the whole stack on one machine: XGI, one XI agent, and a single-node `qwen-7b` pool. It's the quickest way to see a real answer come back through the router.

### 3.1 Get llama.cpp and a model

1. Download a Windows **CPU (AVX2)** release of llama.cpp from the official GitHub releases, and extract it to `C:\qwen-cluster\llama.cpp\`. Check that the folder contains `llama-server.exe` and `rpc-server.exe`.
2. Download `qwen2.5-7b-instruct-q4_k_m.gguf` (~4.7 GB) from `Qwen/Qwen2.5-7B-Instruct-GGUF` on Hugging Face, and put it in `C:\qwen-cluster\models\`. If the download comes split into parts, use the first part's file name in `model_file`.

### 3.2 Write a one-pool config

Save this as `C:\qwen-cluster\local.yaml`:

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

Check it:

```powershell
cd C:\qwen-cluster
.\bin\xgi.exe check-config --config local.yaml
```

### 3.3 Start XGI and XI (two terminals)

Set the same token in both terminals:

```powershell
$env:QWEN_CLUSTER_TOKEN = "local-test-token"
```

**Terminal 1: XGI**

```powershell
cd C:\qwen-cluster
.\bin\xgi.exe serve --config local.yaml
```

**Terminal 2: XI**

```powershell
cd C:\qwen-cluster
.\bin\xi.exe --coordinator http://127.0.0.1:8100 --host 127.0.0.1 --advertise-ip 127.0.0.1 `
  --llama-dir C:\qwen-cluster\llama.cpp --models-dir C:\qwen-cluster\models --log-dir C:\qwen-cluster\logs
```

Within about 5 seconds, terminal 1 logs `node <HOSTNAME> registered`.

### 3.4 Deploy and ask a question

In a third terminal:

```powershell
$env:QWEN_CLUSTER_TOKEN = "local-test-token"
$T = @{ "X-Cluster-Token" = $env:QWEN_CLUSTER_TOKEN }

Invoke-RestMethod http://127.0.0.1:8100/nodes -Headers $T | Format-Table node_id, status, models
Invoke-RestMethod -Method Post http://127.0.0.1:8100/deploy -Headers $T

# Wait until status is "healthy" (the model takes a minute or so to load):
Invoke-RestMethod http://127.0.0.1:8100/groups -Headers $T | Format-Table group_id, status, head_url, error
```

Then send a chat request through the router:

```powershell
$body = @{
  model    = "qwen-7b"
  messages = @(@{ role = "user"; content = "Explain pipeline parallelism in 2 sentences." })
  max_tokens = 200
} | ConvertTo-Json -Depth 5

Invoke-RestMethod -Method Post http://127.0.0.1:8000/v1/chat/completions `
  -ContentType "application/json" -Body $body | ForEach-Object { $_.choices[0].message.content }
```

To see streaming output (SSE) with `curl.exe`:

```powershell
curl.exe -N http://127.0.0.1:8000/v1/chat/completions -H "Content-Type: application/json" `
  -d '{\"model\":\"qwen-7b\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"Hello!\"}]}'
```

Any OpenAI SDK works too: set `base_url="http://127.0.0.1:8000/v1"` and any `api_key`.

Press **Ctrl+C** in terminal 2 to stop XI. It stops `llama-server` on its way out.

### 3.5 Demo without a model (mock binaries)

To try the control plane without downloading anything, point XI at the mocks. Any file ending in `.gguf` in the models folder is enough:

```powershell
New-Item -ItemType Directory -Force C:\temp\mock-models | Out-Null
Set-Content C:\temp\mock-models\qwen2.5-7b-instruct-q4_k_m.gguf "GGUF"

.\bin\xi.exe --coordinator http://127.0.0.1:8100 --host 127.0.0.1 --advertise-ip 127.0.0.1 `
  --models-dir C:\temp\mock-models `
  --llama-server-bin C:\path\to\build\vs\tests\Release\mock-llama-server.exe `
  --rpc-server-bin  C:\path\to\build\vs\tests\Release\mock-rpc-server.exe
```

The mock answers with `Hello from mock llama-server ...` and streams fake tokens.

---

## 4. Run the real cluster

The short version is below. The full procedure, including firewall rules, static IPs, and power settings, is in the [operator guide §7](distributed-qwen-cluster.md#7-installation) and the `node-setup-runbook` skill.

### 4.1 Every node

Run in an **elevated** PowerShell:

```powershell
# 1. Files: C:\qwen-cluster\bin\xi.exe, scripts\install-xi-task.ps1, llama.cpp\ (same build on every node!)
#    Heads only: models\<model_file>.gguf   (workers don't need model files)

# 2. Firewall: only from the cluster subnet, Private profile
New-NetFirewallRule -DisplayName "Qwen RPC"   -Direction Inbound -Protocol TCP -LocalPort 50052 -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24
New-NetFirewallRule -DisplayName "Qwen Agent" -Direction Inbound -Protocol TCP -LocalPort 8200  -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24
New-NetFirewallRule -DisplayName "Qwen Head"  -Direction Inbound -Protocol TCP -LocalPort 8080  -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24

# 3. Token (same long random string everywhere), then the startup task
[Environment]::SetEnvironmentVariable("QWEN_CLUSTER_TOKEN", "<long random string>", "Machine")
C:\qwen-cluster\scripts\install-xi-task.ps1 -Coordinator http://192.168.1.10:8100
Start-ScheduledTask -TaskName QwenNodeAgent
```

`xi.exe` finds its own IP from the route to the coordinator. Use `--advertise-ip` only if the PC has several network adapters. Run `xi.exe --help` to see every flag.

### 4.2 Control machine (wired, if possible)

```powershell
cd C:\qwen-cluster
notepad cluster.yaml                                   # pools, groups, spares (schema: architecture/05 §5)
.\bin\xgi.exe check-config --config cluster.yaml       # validate + node count needed

# Allow clients in (router) and agents in (coordinator):
New-NetFirewallRule -DisplayName "XGI Router"      -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "XGI Coordinator" -Direction Inbound -Protocol TCP -LocalPort 8100 -Action Allow -Profile Private -RemoteAddress 192.168.1.0/24

.\bin\xgi.exe serve --config cluster.yaml              # or run "coordinator" and "router" separately
```

### 4.3 Deploy

```powershell
$T = @{ "X-Cluster-Token" = $env:QWEN_CLUSTER_TOKEN }
$C = "http://192.168.1.10:8100"

Invoke-RestMethod "$C/nodes" -Headers $T | Format-Table node_id, ip, status, models    # all nodes alive?
Invoke-RestMethod -Method Post "$C/deploy" -Headers $T
Invoke-RestMethod "$C/groups" -Headers $T | Format-Table group_id, status, head, workers, error
```

> ⏱ **The first deployment is slow.** Workers receive their weight slices over Wi-Fi, which can take 30–60+ minutes for 72B. Groups stay `loading` meanwhile. Later deploys load from each worker's `rpc-server -c` cache.

Clients use `http://192.168.1.10:8000/v1`. If `router.api_keys` is set, they also send `Authorization: Bearer <key>`.

---

## 5. Day-to-day operations

All coordinator calls except `/health` need `X-Cluster-Token`.

| Task | Command |
|---|---|
| Cluster status | `Invoke-RestMethod "$C/groups" -Headers $T` |
| Node list | `Invoke-RestMethod "$C/nodes" -Headers $T` |
| Router load per pool | `Invoke-RestMethod http://192.168.1.10:8000/health` |
| Take a PC out before maintenance | `Invoke-RestMethod -Method Post "$C/drain/LAB-PC-07" -Headers $T` |
| Put it back | `Invoke-RestMethod -Method Post "$C/undrain/LAB-PC-07" -Headers $T` |
| Restart one group | `Invoke-RestMethod -Method Post "$C/groups/qwen3-30b-a3b-3/restart" -Headers $T` |
| Apply a changed `cluster.yaml` | Restart `xgi`, then `Invoke-RestMethod -Method Post "$C/deploy?force=true" -Headers $T` |
| Stop everything | `Invoke-RestMethod -Method Post "$C/undeploy" -Headers $T` |
| Last 100 lines of a head's llama-server | `Invoke-RestMethod "http://<node-ip>:8200/logs/head?lines=100" -Headers $T` |

### What happens on its own

- **A node stops sending heartbeats for 30 s.** It is marked `dead`, and its group becomes `degraded`. The coordinator stops the survivors, swaps in a spare, and restarts the group.
- **A llama.cpp process exits, or `/health` fails 3 times.** Same repair. Repeated failures back off, up to 5 minutes between attempts.
- **The coordinator restarts.** Agents re-register within about 5 s. Heads keep serving, and the router uses its last known table. Run `POST /deploy` again to restore management ([ADR-006](adr/ADR-006-in-memory-coordinator-state.md)).

### Log files

| Machine | File |
|---|---|
| Control | `logs\xgi.log` (`serve`), or `logs\xgi-coordinator.log` and `logs\xgi-router.log` |
| Node | `logs\xi.log` (agent), `logs\head.log` (llama-server), `logs\rpc.log` (rpc-server) |

Use `--log-level debug` on either executable for more detail.

### Quick troubleshooting

| Symptom | Check |
|---|---|
| Node missing from `/nodes` | `Get-ScheduledTask QwenNodeAgent`. Look for `401` or `register ... failed` in `logs\xi.log`, which usually means a token mismatch or the wrong coordinator URL. |
| Group `unplaced` | Its `error` field says why: no free node has the model file, would consume spares, or not enough nodes. |
| Group `failed` with `rpc-server ... did not open` | Firewall on port 50052, or the wrong `rpc-server.exe` path. Test with `Test-NetConnection <ip> -Port 50052`. |
| Group stuck in `loading` | Expected on the first deploy. Watch `GET /logs/head` on the head node. |
| Router `503 model unavailable` | No `healthy` group in that pool, or a misspelled `model`. |
| Router `429 queue full` | All groups busy and the pool queue is at `max_queue`. Add groups or retry after `Retry-After`. |

For deeper diagnosis, use the `cluster-troubleshooting` skill or the [operator guide §14](distributed-qwen-cluster.md#14-troubleshooting).

> Performance figures in these docs (tokens/s per pool) are **estimates** until benchmarked on your hardware.
