# Qwen Cluster: Documentation

Run Qwen LLMs across about 50 Windows PCs on a Wi-Fi LAN with llama.cpp. Large models are split across machines, many copies serve requests in parallel, and clients reach everything through one OpenAI-compatible endpoint.

> **Status:** implemented in C++ ([ADR-007](adr/ADR-007-cpp-implementation-xgi-xi.md)): **XGI** (`xgi.exe`, the coordinator and router) and **XI** (`xi.exe`, the node agent). The documents below are the specification the code follows. Performance figures are still estimates.

```powershell
cmake --preset vs; cmake --build --preset release; ctest --preset release
```

Code: `src/common` (config, HTTP, sysinfo), `src/xi` (agent, process supervisor), `src/xgi` (coordinator, placement, router), `tests/` (unit, mocks, e2e).

## Start here

| If you want to… | Read |
|---|---|
| Understand what the system is and why it's built this way | [Architecture overview](architecture/01-system-overview.md) |
| Build, test, and run XGI/XI step by step | [HOW_TO_RUN.md](HOW_TO_RUN.md) |
| Install, configure, and run the cluster | [Operator guide](distributed-qwen-cluster.md) |
| Know what speed to expect | [Operator guide §11](distributed-qwen-cluster.md#11-performance-expectations) · [ADR-002](adr/ADR-002-pipeline-not-tensor-parallelism.md) |
| Implement a component | [Component specs](#architecture) + [Protocols & data](architecture/05-protocols-and-data.md) |
| Secure the deployment | [Security](architecture/06-security.md) |
| Work on this repo with Claude | [CLAUDE.md](CLAUDE.md) |

## Architecture

1. [System overview](architecture/01-system-overview.md): context, components, scenarios, deployment
2. [Node Agent](architecture/02-node-agent.md): runs on every PC and supervises llama.cpp
3. [Coordinator](architecture/03-coordinator.md): registry, placement, group lifecycle, failover
4. [Router](architecture/04-router.md): OpenAI API, routing, queueing, streaming
5. [Protocols & data](architecture/05-protocols-and-data.md): ports, JSON schemas, `cluster.yaml`
6. [Security](architecture/06-security.md): threat model, hardening checklist

## Decisions

[ADR index](adr/README.md)

- [ADR-001](adr/ADR-001-llama-cpp-as-inference-engine.md) llama.cpp RPC as the engine
- [ADR-002](adr/ADR-002-pipeline-not-tensor-parallelism.md) Pipeline parallelism plus replicas, not tensor parallelism
- [ADR-003](adr/ADR-003-moe-model-as-default-pool.md) Qwen3-30B-A3B (MoE) as the default pool
- [ADR-004](adr/ADR-004-shared-cluster-token.md) Shared cluster token
- [ADR-005](adr/ADR-005-model-files-relative-to-models-dir.md) Model files pre-staged, relative paths
- [ADR-006](adr/ADR-006-in-memory-coordinator-state.md) In-memory coordinator state
- [ADR-007](adr/ADR-007-cpp-implementation-xgi-xi.md) C++ implementation: XGI and XI

## Claude skills (`.claude/skills/`)

| Skill | Purpose |
|---|---|
| [cluster-capacity-planning](.claude/skills/cluster-capacity-planning/SKILL.md) | Nodes per group, number of groups, estimated tok/s for any model |
| [add-model-pool](.claude/skills/add-model-pool/SKILL.md) | Add or change a pool and keep the docs consistent |
| [node-setup-runbook](.claude/skills/node-setup-runbook/SKILL.md) | Prepare a Windows PC as a node |
| [cluster-troubleshooting](.claude/skills/cluster-troubleshooting/SKILL.md) | Diagnose failing nodes, groups, and requests |
| [write-adr](.claude/skills/write-adr/SKILL.md) | Record an architecture decision |

## At a glance

| | |
|---|---|
| Nodes | ~50 × Windows, ~8 GB free RAM, CPU-only, Wi-Fi |
| Default pool | `qwen3-30b-a3b`: 3 nodes/group, ~5–10 tok/s (estimate) |
| Largest pool | `qwen-72b`: 8 nodes/group, ~0.3–0.6 tok/s (estimate) |
| Endpoint | `http://<control-ip>:8000/v1` (OpenAI-compatible) |
| Ports | 8000 router · 8100 coordinator · 8200 agent · 8080 head · 50052 RPC |
