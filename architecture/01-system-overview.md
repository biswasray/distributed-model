# 01 · System Overview

> Part of the [architecture docs](../README.md#architecture). Read this first.

## 1. Purpose

The Qwen cluster turns a fleet of ordinary Windows PCs on one Wi-Fi network into a **single LLM service** with one OpenAI-compatible endpoint. Models that are too large for any one PC are split across several PCs (**pipeline parallelism**). Throughput comes from running several independent copies of each model (**replicas**).

## 2. Constraints that shape the design

| Constraint | Value | Consequence |
|---|---|---|
| Nodes | ~50 Windows 10/11 PCs | No Linux-only stacks (vLLM, Ray, NCCL) |
| RAM per node | ~8 GB free (plan on ~6.5 GB for weights) | Large models must be split across nodes |
| GPU | ≥300 MB VRAM | Ignored. **CPU-only inference.** |
| Network | Shared Wi-Fi, 5–30 ms latency | Pipeline parallelism only. Minimize hops per token. Cache weights locally. |
| Operators | Non-specialists | One config file, one setup script per node, automatic failover |

## 3. Quality goals (in priority order)

1. **Availability.** The service keeps answering when individual PCs go offline.
2. **Operability.** Nodes are set up once and then managed centrally.
3. **Throughput.** Total requests served across the cluster.
4. **Latency.** Tokens per second for a single request. It is limited by physics (see [ADR-002](../adr/ADR-002-pipeline-not-tensor-parallelism.md)), so it comes last.

## 4. Context diagram

```
   ┌──────────────┐         HTTP (OpenAI API)          ┌──────────────────────────┐
   │   Clients    │ ─────────────────────────────────► │      Qwen Cluster        │
   │ chat UIs,    │ ◄───────── SSE token stream ────── │  (50 Windows PCs, Wi-Fi) │
   │ scripts, IDE │                                    └────────────┬─────────────┘
   └──────────────┘                                                 │ one-time
                                                                    ▼
                                                   ┌───────────────────────────────┐
                                                   │ Hugging Face (GGUF downloads) │
                                                   └───────────────────────────────┘
```

## 5. Container / component view

```
                         ┌───────────────────────────────┐
   Clients ──HTTP:8000──►│ Router  (XGI, xgi.exe)        │
                         │  pool lookup · least-busy     │
                         │  queue · SSE streaming        │
                         └──────┬──────────────▲─────────┘
               proxied request  │              │ GET /groups (poll 3 s)
                                │       ┌──────┴─────────────────────────┐
                                │       │ Coordinator (XGI) :8100        │
                                │       │  node registry · placement     │
                                │       │  group lifecycle · failover    │
                                │       └──────▲──────────────┬──────────┘
                                │   register / │              │ start/stop commands
                                │   heartbeat  │              ▼
            ┌───────────────────┼──────────────┴──────────────────────────────────┐
            │  Every PC: Node Agent :8200  (process supervisor for llama.cpp)     │
            │                                                                     │
            │   Group = 1 head + (k−1) workers                                    │
            │   ┌────────────────┐  RPC :50052  ┌────────────┐      ┌───────────┐ │
            └──►│ llama-server   │─────────────►│ rpc-server │─────►│rpc-server │ │
                │ (head) :8080   │◄─────────────│ (worker)   │◄─────│ (worker)  │ │
                └────────────────┘              └────────────┘      └───────────┘ │
            └─────────────────────────────────────────────────────────────────────┘
```

| Component | Runs on | Port | Responsibility | Spec |
|---|---|---|---|---|
| Router (`xgi router`) | 1 machine (wired, if possible) | 8000 | Public API, routing, queueing, streaming | [04-router.md](04-router.md) |
| Coordinator (`xgi coordinator`) | Same machine as the router | 8100 | Cluster state, placement, lifecycle, failover | [03-coordinator.md](03-coordinator.md) |
| Node Agent (`xi.exe`) | Every node | 8200 | Registers the node, sends heartbeats, supervises llama.cpp processes | [02-node-agent.md](02-node-agent.md) |
| `llama-server` | Head node of each group | 8080 | Inference, the first layer slice, HTTP completion API | external (llama.cpp) |
| `rpc-server` | Worker nodes | 50052 | Remote compute backend holding a slice of layers | external (llama.cpp) |

The control plane is implemented in C++ ([ADR-007](../adr/ADR-007-cpp-implementation-xgi-xi.md)) and runs on Windows, Linux and macOS ([ADR-008](../adr/ADR-008-cross-platform-build.md)). **XGI** (`xgi`, Extended General Intelligence) is the cluster core: the coordinator and router, run separately or together with `xgi serve`. **XI** (`xi`, Extended Interface) is the node agent. The fleet in this document is Windows, but the control machine or individual nodes may also be Linux or macOS. Keep each group on one OS. Inference itself is always llama.cpp.

## 6. Core concepts and vocabulary

| Term | Definition |
|---|---|
| **Node** | One PC running the Node Agent. Identified by `node_id` (the hostname by default). |
| **Pool** | All groups serving one model name, e.g. `qwen3-30b-a3b`. Clients choose a pool with the `model` field. |
| **Group** | One complete running copy of a model: **1 head + (`nodes_per_group` − 1) workers**. |
| **Head** | The node that runs `llama-server` and holds the GGUF file on disk. It is the group's HTTP entry point. |
| **Worker** | A node that runs `rpc-server` and holds a slice of the model in RAM. Its tensor cache is on local disk. |
| **Spare** | A healthy node that is deliberately left unassigned so it can replace a failed one. |
| **Slot** | One concurrent request inside a head (`llama-server -np`). |

## 7. Main runtime scenarios

### 7.1 Inference request (happy path)

```
Client        Router                Head (llama-server)     Worker 1 … Worker k−1
  │ POST /v1/chat/completions {model}  │                        │
  │──────────────►│                    │                        │
  │               │ pick pool → least-busy healthy group        │
  │               │──── proxy ────────►│                        │
  │               │                    │── activations ────────►│ (layer slices, in order)
  │               │                    │◄────── result ─────────│
  │               │◄── SSE tokens ─────│   (repeats per token)  │
  │◄── SSE ───────│                    │                        │
```

### 7.2 Cluster deployment

1. The agents register when they start (`POST /register`) and then send heartbeats every 5 seconds.
2. The operator calls `POST /deploy` on the coordinator.
3. The coordinator **plans placement** from `cluster.yaml`. It picks heads from nodes that already have the model file, and picks workers by free RAM ([03-coordinator.md §4](03-coordinator.md#4-placement-algorithm)).
4. For each group: start the workers' `rpc-server`, wait for their TCP ports, start the head's `llama-server` with `--rpc`, and poll `/health` until it returns 200.
5. The group becomes `healthy`, and the router starts sending traffic to it.

### 7.3 Node failure

1. A worker misses heartbeats for `node_timeout_s` (30 seconds).
2. The coordinator marks the node `dead` and its group `degraded`. The router stops routing new requests to that group.
3. The coordinator stops the group's surviving processes, swaps in a spare (or any free node that qualifies), and restarts the group ([03-coordinator.md §6](03-coordinator.md#6-failure-handling)).
4. If no replacement is available, the group becomes `unplaced`. The coordinator retries every monitor cycle until enough nodes are free.

## 8. Deployment view

```
               ┌──────── Wired (recommended) ───────┐
               │  Control machine                   │
               │   Router :8000 · Coordinator :8100 │
               └───────────────┬────────────────────┘
                               │ Ethernet / Wi-Fi AP
      ┌────────────────────────┼────────────────────────┐
      ▼                        ▼                        ▼
  AP 1 (5 GHz)             AP 2 (5 GHz)             AP 3 (5 GHz)
  groups 1–5               groups 6–10              groups 11–16 + spares
```

- Keep all nodes of a group **on the same access point**. Every token crosses every hop in the group.
- Give every node a **static IP or a DHCP reservation**. Group membership uses IP addresses.

## 9. Cross-cutting concerns

| Concern | Approach |
|---|---|
| Configuration | A single `cluster.yaml` on the control machine. Agents only take CLI flags. See [05-protocols-and-data.md §5](05-protocols-and-data.md#5-configuration-schema). |
| Authentication | A shared token in the `X-Cluster-Token` header on all control-plane calls ([ADR-004](../adr/ADR-004-shared-cluster-token.md)). Optional client API keys on the router. |
| Logging | Each service logs to `logs\<name>.log`. Each llama.cpp process logs to `logs\rpc.log` or `logs\head.log` on its node. |
| State | Kept in coordinator memory and rebuilt from agent registrations after a restart ([ADR-006](../adr/ADR-006-in-memory-coordinator-state.md)). |
| Versioning | All nodes run the **same llama.cpp build**. The RPC protocol is not stable across versions. |

## 10. Read next

- [02 · Node Agent](02-node-agent.md)
- [03 · Coordinator](03-coordinator.md)
- [04 · Router](04-router.md)
- [05 · Protocols & data](05-protocols-and-data.md)
- [06 · Security](06-security.md)
- [Decision records](../adr/)
