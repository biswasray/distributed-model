# Distributed Qwen Inference Cluster

> **Operator guide**: running Qwen models across 50 Windows machines on a shared Wi-Fi network.
> See the [docs index](README.md) for the architecture specs and decision records.

| | |
|---|---|
| **Status** | Implemented in C++ as **XGI** (`xgi.exe`: coordinator + router) and **XI** (`xi.exe`: node agent). See [ADR-007](adr/ADR-007-cpp-implementation-xgi-xi.md). Not yet benchmarked on real hardware. |
| **Target hardware** | 50 × Windows PCs, ~8 GB free RAM each, ≥300 MB GPU (not used), connected over Wi-Fi |
| **Target models** | Qwen2.5-72B-Instruct, Qwen3-30B-A3B (MoE), Qwen2.5-7B-Instruct |
| **Inference engine** | [llama.cpp](https://github.com/ggml-org/llama.cpp) (`llama-server` + `rpc-server`) |
| **Last updated** | 2026-11-10 |

---

## Table of contents

1. [Overview](#1-overview)
2. [Key concepts](#2-key-concepts)
3. [Hardware analysis](#3-hardware-analysis)
4. [Deployment options](#4-deployment-options)
5. [System architecture](#5-system-architecture)
6. [Components](#6-components)
7. [Installation](#7-installation)
8. [Configuration](#8-configuration)
9. [Running the cluster](#9-running-the-cluster)
10. [API reference](#10-api-reference)
11. [Performance expectations](#11-performance-expectations)
12. [Monitoring and operations](#12-monitoring-and-operations)
13. [Security](#13-security)
14. [Troubleshooting](#14-troubleshooting)
15. [Limitations and future work](#15-limitations-and-future-work)
16. [Glossary](#16-glossary)

---

## 1. Overview

The cluster combines many ordinary Windows PCs into one LLM service with **a single OpenAI-compatible API endpoint**. Clients such as chat UIs, scripts, and IDE plugins send requests to one URL. The cluster sends each request to a group of machines that runs the model.

### Goals

- Run models that are **too large for any single machine**, such as Qwen 72B (about 47 GB at Q4), by splitting them across several machines.
- Serve **many users at once** by running several independent copies of a model, each on its own group of machines.
- Keep it **simple to operate**: one setup script per node, one coordinator, and one config file.
- Work on **Windows over Wi-Fi**, without requiring Linux, CUDA, or wired networking.

### Non-goals

- Making *a single response* faster by adding more machines. Section 2 explains why this isn't possible on this hardware.
- Training or fine-tuning models.

---

## 2. Key concepts

### 2.1 Pipeline parallelism (used by this system)

The model's layers are split into consecutive slices, and each machine holds one slice:

```
Token ──► [PC 1: layers 1–10] ──► [PC 2: layers 11–20] ──► … ──► [PC 8: layers 71–80] ──► next token
```

- ✅ The combined RAM of the group holds the model, so 8 × 6.5 GB fits a 47 GB model.
- ❌ For a single request, **only one machine works at any moment**. Per token, the group still reads the whole model from RAM, and every hop adds network delay. More machines add capacity, not speed.

### 2.2 Tensor parallelism (not used)

Each layer is split across machines, and all of them compute at the same time. This can make each token faster, but it needs roughly two network synchronizations per layer (about 160 per token for a model with 80 layers). It only pays off with very fast interconnects such as NVLink, InfiniBand, or 100 GbE. **Over Wi-Fi, it is slower than pipeline parallelism.**

### 2.3 Data parallelism / replicas (used by this system)

Several **independent copies** of the model run at the same time, and a router hands each request to a free copy. Total throughput scales almost linearly with the number of copies.

### 2.4 Mixture of experts (MoE)

MoE models such as **Qwen3-30B-A3B** have many parameters in total (30B) but use only a few for each token (about 3B active). The memory needed to store the model stays large, but the RAM read for each token is small. That keeps a split MoE model fast, which makes MoE the best fit for this hardware.

### 2.5 Quantization

Weights are stored at lower precision (GGUF formats) to save memory:

| Format | Bits/weight (approx.) | Quality | Use |
|---|---|---|---|
| Q4_K_M | ~4.8 | Good, the usual default | **Recommended** |
| Q5_K_M | ~5.7 | Better | If RAM allows |
| Q8_0 | ~8.5 | Close to original | Rarely worth it here |
| F16 | 16 | Original | Not feasible |

---

## 3. Hardware analysis

### 3.1 Per-node budget

| Resource | Value | Notes |
|---|---|---|
| Free RAM | ~8 GB | Plan on **~6.5 GB** for model weights. The rest covers the OS, the KV cache, and buffers. |
| GPU | ≥300 MB VRAM | Too small to help. **All inference runs on the CPU.** |
| Network | Wi-Fi (shared) | High and variable latency (5–30 ms). Bandwidth is shared by every device on the access point. |
| RAM bandwidth | ~20–40 GB/s | **This sets the token speed.** |

### 3.2 How many nodes each model needs

| Model | Quant | File size | Nodes per group | Groups possible (50 nodes) |
|---|---|---|---|---|
| Qwen2.5-72B-Instruct | Q4_K_M | ~47 GB | **8** | 6 |
| Qwen2.5-72B-Instruct | Q8_0 | ~77 GB | 12–13 | 3 |
| Qwen3-30B-A3B (MoE) | Q4_K_M | ~18.6 GB | **3** | 16 |
| Qwen2.5-14B-Instruct | Q4_K_M | ~9 GB | 2 | 25 |
| Qwen2.5-7B-Instruct | Q4_K_M | ~4.7 GB | **1** | 50 |

> Leave 1–2 nodes as spares in each deployment so the cluster can keep serving when a machine goes offline.

---

## 4. Deployment options

### Option A: Qwen 72B replicas

- **Layout:** 6 groups × 8 nodes = 48 nodes, plus 2 spares.
- **Speed:** ~0.3–0.6 tokens/s per request.
- **Concurrency:** 6 requests at once.
- **When to use:** you need true 72B quality and can accept very slow answers (a 300-word reply takes 10–20 minutes).

### Option B: Qwen3-30B-A3B MoE replicas ⭐ Recommended

- **Layout:** 16 groups × 3 nodes = 48 nodes, plus 2 spares.
- **Speed:** ~5–10 tokens/s per request (estimate).
- **Concurrency:** 16 requests at once.
- **When to use:** the default choice. Quality is close to 72B for most tasks, at usable speed.

### Option C: Qwen 7B on every node

- **Layout:** 50 independent single-node servers.
- **Speed:** ~5–8 tokens/s per request.
- **Concurrency:** 50 requests at once.
- **When to use:** high volume of simple tasks such as classification, extraction, or short answers.

### Option D: Hybrid

Mix the options behind the same router and pick the model per request with the `model` field. For example:

| Pool | Nodes | Purpose |
|---|---|---|
| `qwen-72b` | 1 group × 8 = 8 | Hardest tasks, slow |
| `qwen3-30b-a3b` | 10 groups × 3 = 30 | Default, general use |
| `qwen-7b` | 10 × 1 = 10 | Fast, simple tasks |
| spare | 2 | Failover |

---

## 5. System architecture

```
                         ┌──────────────────────────────┐
   Clients  ───HTTP───►  │        Router / API          │  :8000  (OpenAI-compatible)
 (chat UI, scripts)      │  - picks a pool by "model"   │
                         │  - picks least-busy group    │
                         │  - streams the response back │
                         └──────────────┬───────────────┘
                                        │ reads cluster state
                         ┌──────────────▼───────────────┐
                         │         Coordinator          │  :8100
                         │  - node registry/heartbeats  │
                         │  - assigns nodes to groups   │
                         │  - starts/stops group heads  │
                         └──────┬───────────────┬───────┘
                 heartbeat/cmds │               │
          ┌─────────────────────┘               └──────────────────────┐
          ▼                                                            ▼
 ┌──────────────── Group 1 (e.g. 3 nodes, Qwen3-30B-A3B) ─┐   ┌─── Group N ───┐
 │  ┌──────────────┐   RPC   ┌────────────┐  RPC  ┌──────┐ │   │      …        │
 │  │ Head node    │ ──────► │ rpc-server │ ────► │ rpc- │ │   └───────────────┘
 │  │ llama-server │ ◄────── │  (node 2)  │ ◄──── │server│ │
 │  │  :8080       │         └────────────┘       │(n 3) │ │
 │  └──────────────┘                              └──────┘ │
 └────────────────────────────────────────────────────────┘
       Every node also runs the Node Agent (:8200)
```

### Request flow

1. A client sends `POST /v1/chat/completions` to the router with `"model": "qwen3-30b-a3b"`.
2. The router looks up the healthy groups in that pool and picks the one with the fewest active requests.
3. The router forwards the request to that group's **head node** (`llama-server`).
4. The head node runs its own layers, then sends activations to the next nodes' `rpc-server`s in order.
5. Tokens stream back through the head node and the router to the client.

---

## 6. Components

### 6.1 Node Agent (on every machine)

**XI** (`xi.exe`, port **8200**), a single native executable that:

- reports hardware to the coordinator at startup: hostname, IP, free RAM, CPU cores, and the GGUF files in its models folder;
- sends a **heartbeat** every 5 seconds;
- starts and stops `rpc-server` or `llama-server` when the coordinator asks;
- reports local model shards and cache state.

### 6.2 llama.cpp `rpc-server` (worker nodes)

Exposes the machine's CPU and RAM as a remote compute backend.

```powershell
rpc-server.exe -H 0.0.0.0 -p 50052 -c
```

- `-c` turns on the **local tensor cache**. After the first load, weights are read from local disk instead of being sent again over Wi-Fi. This is essential on this network.

### 6.3 llama.cpp `llama-server` (head node of each group)

Holds the full GGUF file, runs the first slice of layers, and sends the remaining layers to the group's RPC workers.

```powershell
llama-server.exe `
  -m C:\qwen-cluster\models\Qwen3-30B-A3B-Q4_K_M.gguf `
  --rpc 192.168.1.21:50052,192.168.1.22:50052 `
  -ngl 99 `
  --host 0.0.0.0 --port 8080 `
  -c 4096 --threads 8
```

| Flag | Meaning |
|---|---|
| `--rpc` | Comma-separated list of the group's RPC workers |
| `-ngl 99` | Offload all layers to the backends (the RPC workers count as devices) |
| `--tensor-split` | Optional. Shares of layers per device, e.g. `1,1,1` |
| `-c 4096` | Context length. A longer context needs more RAM for the KV cache. |
| `--threads` | CPU threads, usually the number of physical cores |

> The head node needs the **whole GGUF file on its own disk** (to read it), but only enough **RAM** for its own slice.

### 6.4 Coordinator

Part of **XGI** (`xgi coordinator`, port **8100**). It:

- keeps the **node registry** (alive or dead, resources, assigned role);
- assigns nodes to groups from `cluster.yaml` (section 8), preferring nodes with more RAM as heads;
- starts each group: workers first, then the head;
- detects failed nodes (no heartbeat for 30 seconds), marks the group *degraded*, and rebuilds it with a spare node.

### 6.5 Router / API gateway

Part of **XGI** (`xgi router`, port **8000**). It:

- exposes an OpenAI-compatible API (section 10);
- maps the `model` name to a pool, then routes to the **least-busy healthy group**;
- queues requests when every group is busy, with a configurable maximum queue length and timeout;
- streams responses with Server-Sent Events.

The coordinator and router can run on the same machine, in one process with `xgi serve`. Ideally that machine has a **wired** connection to the access point.

---

## 7. Installation

### 7.1 Prerequisites (all nodes)

- Windows 10/11 64-bit
- No runtime to install: `xi.exe` and `xgi.exe` are self-contained (static C++ runtime)
- Free disk space: **50 GB** on head nodes for 72B, **20 GB** for MoE heads, and space for each worker's cached shard
- A **static IP or DHCP reservation** for every node (set this on the router or access point)
- All nodes on the **same subnet**

### 7.2 Install llama.cpp

Download a prebuilt Windows release (CPU build, AVX2) from the official llama.cpp GitHub Releases page and extract it to:

```
C:\qwen-cluster\llama.cpp\
```

Make sure the release you pick **includes `rpc-server.exe`**, and use **the same llama.cpp version on every node**. The RPC protocol isn't guaranteed to work across versions.

### 7.3 Open firewall ports

Run in an **elevated** PowerShell on each node:

```powershell
New-NetFirewallRule -DisplayName "Qwen RPC"   -Direction Inbound -Protocol TCP -LocalPort 50052 -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "Qwen Agent" -Direction Inbound -Protocol TCP -LocalPort 8200  -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "Qwen Head"  -Direction Inbound -Protocol TCP -LocalPort 8080  -Action Allow -Profile Private
```

Set the Wi-Fi network profile to **Private**. These rules don't open the ports on public networks.

### 7.4 Download models

Download GGUF files **only to head nodes**. Workers receive their slices over RPC on the first load and cache them.

| Model | Source (Hugging Face) | File |
|---|---|---|
| Qwen2.5-72B-Instruct | `Qwen/Qwen2.5-72B-Instruct-GGUF` | `*q4_k_m*.gguf` (split into parts) |
| Qwen3-30B-A3B | `Qwen/Qwen3-30B-A3B-GGUF` | `*Q4_K_M.gguf` |
| Qwen2.5-7B-Instruct | `Qwen/Qwen2.5-7B-Instruct-GGUF` | `*q4_k_m*.gguf` |

To save Wi-Fi bandwidth, download once to a USB drive or a wired machine and copy the files to the head nodes. Don't have 16 heads download the same file over Wi-Fi.

### 7.5 Install the cluster services

Build once, on any Windows machine with Visual Studio 2022 or later (the "Desktop development with C++" workload) and internet access for the first configure:

```powershell
cmake --preset vs
cmake --build --preset release
ctest --preset release                      # unit + end-to-end tests (mock llama.cpp)
cmake --install build\vs --config Release --prefix C:\qwen-cluster
```

This puts `bin\xi.exe`, `bin\xgi.exe`, `cluster.yaml` (an example) and `scripts\install-xi-task.ps1` under `C:\qwen-cluster`. Copy that folder to every node. Nodes only need `bin\xi.exe` and the script. The control machine needs `bin\xgi.exe` and `cluster.yaml`.

> If Windows **Smart App Control** or WDAC blocks the unsigned executables, sign them or allow them through your policy. Until it has checked a newly built file, Smart App Control blocks it with "An Application Control policy has blocked this file".

### 7.6 Run the node agent as a startup service

Set the token as a machine environment variable, then register XI to start at boot with Task Scheduler. Run this in an **elevated** PowerShell:

```powershell
[Environment]::SetEnvironmentVariable('QWEN_CLUSTER_TOKEN', '<long random string>', 'Machine')
C:\qwen-cluster\scripts\install-xi-task.ps1 -Coordinator http://192.168.1.10:8100
Start-ScheduledTask -TaskName QwenNodeAgent
```

The script registers `C:\qwen-cluster\bin\xi.exe --coordinator ... --llama-dir C:\qwen-cluster\llama.cpp --models-dir C:\qwen-cluster\models --log-dir C:\qwen-cluster\logs` as the `QwenNodeAgent` task (at startup, as SYSTEM, restarted if it exits). Run `xi --help` to see every flag.

---

## 8. Configuration

All cluster layout lives in `cluster.yaml` on the coordinator machine.

> Full schema and validation rules: [architecture/05-protocols-and-data.md §5](architecture/05-protocols-and-data.md#5-configuration-schema).

```yaml
cluster_token: ""              # better: set env QWEN_CLUSTER_TOKEN on every machine

coordinator:
  host: 0.0.0.0
  port: 8100
  heartbeat_interval_s: 5
  node_timeout_s: 30

router:
  host: 0.0.0.0
  port: 8000
  max_queue: 100
  request_timeout_s: 1800      # 72B answers can take many minutes

llama_cpp:
  rpc_cache: true              # pass -c to rpc-server
  extra_head_args: []          # appended to every llama-server
# The llama.cpp folder, ports and threads are per-node xi.exe flags (--llama-dir, --rpc-port, --head-port).

pools:
  - name: qwen3-30b-a3b
    model_file: Qwen3-30B-A3B-Q4_K_M.gguf      # relative to each head's models dir
    nodes_per_group: 3
    groups: 10
    context: 4096

  - name: qwen-72b
    model_file: qwen2.5-72b-instruct-q4_k_m-00001-of-00012.gguf
    nodes_per_group: 8
    groups: 1
    context: 2048

  - name: qwen-7b
    model_file: qwen2.5-7b-instruct-q4_k_m.gguf
    nodes_per_group: 1
    groups: 10
    context: 8192

spares: 2

# Optional: pin specific machines to roles
pinned:
  - host: LAB-PC-01
    role: head
    pool: qwen-72b
```

| Key | Description |
|---|---|
| `model_file` | GGUF file name **relative to each head's `--models-dir`**. Only nodes that already hold this file can be heads. |
| `nodes_per_group` | Machines that share one copy of the model (pipeline length) |
| `groups` | Number of copies of that model |
| `context` | Maximum context length per request. Larger values use more RAM on every node. |
| `spares` | Nodes kept idle for automatic failover |

---

## 9. Running the cluster

### 9.1 Startup order

1. Power on all nodes. The agents start automatically and register with the coordinator.
2. Check the config, then start the coordinator (or `xgi serve`, which runs the coordinator and the router together):
   ```powershell
   cd C:\qwen-cluster
   .\bin\xgi.exe check-config --config cluster.yaml
   .\bin\xgi.exe coordinator --config cluster.yaml
   ```
3. Check that all nodes are registered:
   ```powershell
   curl.exe -H "X-Cluster-Token: $env:QWEN_CLUSTER_TOKEN" http://192.168.1.10:8100/nodes
   ```
4. Deploy the pools. Workers start first, then heads load their models:
   ```powershell
   curl.exe -X POST -H "X-Cluster-Token: $env:QWEN_CLUSTER_TOKEN" http://192.168.1.10:8100/deploy
   ```
   > ⏱ **The first deployment is slow.** Weights stream to workers over Wi-Fi, which can take 30–60+ minutes for 72B. Later starts read from each worker's local cache (`-c`) and take a few minutes.
5. Start the router:
   ```powershell
   .\bin\xgi.exe router --config cluster.yaml
   ```

### 9.2 Quick test

```powershell
curl http://192.168.1.10:8000/v1/chat/completions `
  -H "Content-Type: application/json" `
  -d '{\"model\":\"qwen3-30b-a3b\",\"messages\":[{\"role\":\"user\",\"content\":\"Hello!\"}]}'
```

### 9.3 Manual mode (without coordinator)

To test a single group by hand:

```powershell
# On worker nodes 192.168.1.21 and 192.168.1.22
C:\qwen-cluster\llama.cpp\rpc-server.exe -H 0.0.0.0 -p 50052 -c

# On the head node
C:\qwen-cluster\llama.cpp\llama-server.exe -m C:\qwen-cluster\models\Qwen3-30B-A3B-Q4_K_M.gguf `
  --rpc 192.168.1.21:50052,192.168.1.22:50052 -ngl 99 --host 0.0.0.0 --port 8080 -c 4096
```

Then open `http://<head-ip>:8080` in a browser to use llama-server's built-in chat UI.

---

## 10. API reference

The router implements the core of the OpenAI API, so existing clients work by changing the base URL to `http://<router-ip>:8000/v1`.

### `GET /v1/models`

Lists the pools that have at least one healthy group.

```json
{ "object": "list",
  "data": [
    { "id": "qwen3-30b-a3b", "object": "model" },
    { "id": "qwen-72b",      "object": "model" },
    { "id": "qwen-7b",       "object": "model" } ] }
```

### `POST /v1/chat/completions`

| Field | Type | Notes |
|---|---|---|
| `model` | string | Pool name from `cluster.yaml` |
| `messages` | array | OpenAI chat format |
| `stream` | bool | `true` for Server-Sent Events. Recommended for slow pools. |
| `max_tokens` | int | Limit this, especially on `qwen-72b` |
| `temperature`, `top_p` | float | Passed through to llama.cpp |

### `POST /v1/completions`

Plain-text completion, passed through to the head node.

### Cluster endpoints (coordinator, `:8100`)

| Method | Path | Description |
|---|---|---|
| GET | `/health` | Coordinator liveness (no token needed) |
| GET | `/nodes` | All nodes, status, RAM, and role |
| GET | `/groups` | Groups, members, health, and active requests |
| POST | `/deploy` | Build and start all pools from `cluster.yaml` (`?force=true` to redeploy) |
| POST | `/undeploy` | Stop all groups |
| POST | `/groups/{id}/restart` | Restart one group |
| POST | `/drain/{node_id}` · `/undrain/{node_id}` | Take a node out of service gracefully, or return it |

All coordinator endpoints except `/health` need the header `X-Cluster-Token`. Full schemas are in [architecture/05-protocols-and-data.md](architecture/05-protocols-and-data.md).

### Python client example

```python
from openai import OpenAI

client = OpenAI(base_url="http://192.168.1.10:8000/v1", api_key="not-needed")

stream = client.chat.completions.create(
    model="qwen3-30b-a3b",
    messages=[{"role": "user", "content": "Explain pipeline parallelism in 3 sentences."}],
    stream=True,
)
for chunk in stream:
    print(chunk.choices[0].delta.content or "", end="", flush=True)
```

---

## 11. Performance expectations

These are estimates for CPU-only nodes over Wi-Fi. Real numbers depend on CPU, RAM speed, and Wi-Fi conditions. Measure with `llama-bench` and your own test prompts.

| Pool | Nodes/group | Tokens/s per request | 300-word answer | Concurrent requests (50 nodes) |
|---|---|---|---|---|
| Qwen 72B Q4 | 8 | ~0.3–0.6 | ~10–20 min | 6 |
| Qwen3-30B-A3B Q4 | 3 | ~5–10 | ~40–80 s | 16 |
| Qwen 7B Q4 | 1 | ~5–8 | ~50–80 s | 50 |

### Why 72B is slow

```
Time per token ≈ (model bytes ÷ RAM bandwidth) + (hops × Wi-Fi latency)
               ≈ (47 GB ÷ ~25 GB/s) + (8 × ~15 ms)
               ≈ 1.9 s + 0.12 s  ≈  ~2 s/token
```

Adding more machines to a group **doesn't** shrink the first term. The same 47 GB still has to be read, just spread across more machines. More machines only let you run **more copies**.

### Tips for better performance

- **Wired Ethernet** for at least the router, coordinator, and head nodes, even through a cheap switch.
- **5 GHz Wi-Fi**, and spread the nodes across several access points.
- Put each group's nodes **on the same access point**.
- Use **dual-channel RAM** on the machines. It roughly doubles memory bandwidth.
- Keep `context` small. The KV cache uses RAM on every node in the group.
- Close other memory-heavy apps on the nodes.

---

## 12. Monitoring and operations

- **Dashboard:** `GET /groups` on the coordinator shows each group's health and load. You can connect it to Grafana or poll it with a script.
- **Logs:** each agent writes to `C:\qwen-cluster\logs\xi.log`, and its llama.cpp output goes to `logs\head.log` or `logs\rpc.log`. The last 500 lines are also available from `GET /logs/{role}` on the agent. On the control machine, XGI writes `logs\xgi-coordinator.log` and `logs\xgi-router.log` (or `logs\xgi.log` for `xgi serve`).
- **Failover:** when a node stops sending heartbeats, the coordinator marks its group *degraded*, drains in-flight requests, replaces the node with a spare, and restarts the group.
- **Maintenance:** call `POST /drain/{host}` before rebooting or updating a machine.
- **Upgrades:** update llama.cpp on **all nodes at the same time**. Mixed versions can break RPC.

---

## 13. Security

> ⚠️ **llama.cpp's `rpc-server` has no authentication or encryption.** Anyone who can reach port 50052 can run work on that machine and send it data.

- Run the cluster **only on a trusted, private network**. Never expose ports 50052, 8080, 8100, or 8200 to the internet.
- Allow those ports only on the **Private** firewall profile, and preferably only from cluster IPs (`-RemoteAddress 192.168.1.0/24`).
- If the router must be reachable more widely, put it behind a reverse proxy with authentication (an API key) and HTTPS.
- Prompts and responses travel unencrypted on the LAN. Don't send sensitive data unless the network is secured.

---

## 14. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Head fails with "failed to connect to RPC server" | Firewall, wrong IP, or worker not running | Check `rpc-server` is running, test with `Test-NetConnection <ip> -Port 50052`, and check the firewall rule |
| Model loading takes an hour | First transfer of weights over Wi-Fi | Expected once. Make sure `-c` is set on workers so later loads use the cache. |
| Out-of-memory or crash on a worker | Slice plus KV cache bigger than free RAM | Add a node to the group, lower `context`, or use `--tensor-split` to give that node fewer layers |
| Very slow or unstable token speed | Wi-Fi congestion or interference | Use 5 GHz, fewer nodes per access point, wired heads, and run deployments outside busy hours |
| Group repeatedly marked degraded | A node is sleeping or roaming between access points | Turn off sleep and hibernation, set the Wi-Fi adapter to "maximum performance", and fix the IP |
| Strange errors after update | Mixed llama.cpp versions | Use the same version everywhere |
| Router returns 503 | No healthy groups in the pool | Check `GET /groups` and the head node logs |

### Useful commands

```powershell
Test-NetConnection 192.168.1.21 -Port 50052        # is the worker reachable?
Get-Process rpc-server, llama-server                 # is it running?
.\llama-bench.exe -m <model.gguf>                    # local speed baseline
```

---

## 15. Limitations and future work

### Known limitations

- Speed per request is limited by RAM bandwidth and Wi-Fi latency, not by the number of machines.
- One failing node takes down its whole group until the coordinator rebuilds it.
- The first model deployment over Wi-Fi is slow.
- The cluster has no authentication between nodes.

### Possible improvements

- **Speculative decoding:** a small Qwen draft model on the head proposes tokens that the big model checks in batches. This can give 1.5–2× speed on the 72B pool.
- **Pre-sharding:** copy each worker's slice ahead of time (USB or wired) so the first deployment skips the Wi-Fi transfer.
- **Prompt caching** at the router, so repeated system prompts are reused.
- **Wired backbone:** even a cheap gigabit switch for the head nodes greatly reduces latency.
- **Larger MoE models** such as Qwen3-235B-A22B (~140 GB at Q4, about 22 nodes per group). Quality is higher than 72B, at a similar or better speed per token.

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **GGUF** | The file format llama.cpp uses for quantized models |
| **Quantization** | Storing weights at lower precision (e.g. 4-bit) to save memory |
| **Pipeline parallelism** | Splitting a model's layers across machines in sequence |
| **Tensor parallelism** | Splitting each layer's math across machines in parallel |
| **Replica / group** | One complete running copy of a model, on one or more machines |
| **Pool** | All groups serving the same model name |
| **Head node** | The machine running `llama-server` for a group. It receives requests. |
| **Worker node** | A machine running `rpc-server` and holding part of the model |
| **KV cache** | Memory that holds the conversation context while generating |
| **MoE** | Mixture of experts: a model that uses only a few of its parameters for each token |
| **Tokens/s** | Generation speed. One token is about ¾ of an English word. |
