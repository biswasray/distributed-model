# CLAUDE.md

Guidance for Claude (and any AI assistant) working in this repository.

## What this project is

**Qwen Cluster**: a system that runs Qwen LLMs across about 50 Windows PCs on a shared Wi-Fi LAN. Each PC has about 8 GB free RAM and no usable GPU. Large models are split across PCs with **llama.cpp RPC (pipeline parallelism)**. Throughput comes from **many independent replicas (groups)** behind one **OpenAI-compatible router**.

**Current phase: implementation.** The architecture is specified in Markdown, and the control plane is implemented in C++20 with CMake and MSVC ([ADR-007](adr/ADR-007-cpp-implementation-xgi-xi.md)):

- **XGI** (`xgi.exe`, Extended General Intelligence): the coordinator and router (`xgi coordinator | router | serve | check-config`)
- **XI** (`xi.exe`, Extended Interface): the node agent

Inference is still llama.cpp. XGI orchestrates it and is not an inference engine.

> Code must follow the docs. If you change behaviour or a contract, update the docs in the order given in "Sources of truth" below. Build and test with `cmake --preset vs`, `cmake --build --preset release`, and `ctest --preset release` (unit tests plus an end-to-end run with mock llama.cpp binaries).

## Repository layout

```
docs/
├── CLAUDE.md                          ← this file
├── README.md                          ← docs index; start here
├── distributed-qwen-cluster.md        ← operator guide: install, configure, run, troubleshoot
├── architecture/
│   ├── 01-system-overview.md          ← context, components, scenarios, deployment view
│   ├── 02-node-agent.md               ← agent spec (CLI, API, process supervision)
│   ├── 03-coordinator.md              ← registry, placement algorithm, state machine, failover
│   ├── 04-router.md                   ← routing, queueing, proxying, client auth
│   ├── 05-protocols-and-data.md       ← ports, JSON schemas, cluster.yaml schema
│   └── 06-security.md                 ← threat model and hardening checklist
├── adr/                               ← Architecture Decision Records (immutable once accepted)
├── .claude/skills/                    ← project skills (see below)
├── CMakeLists.txt · CMakePresets.json · cmake/Dependencies.cmake   ← build; deps pinned by SHA-256
├── src/common/                        ← config (cluster.yaml), HTTP helpers, logging, sysinfo
├── src/xi/                            ← XI: agent API, command lines, Windows process supervisor
├── src/xgi/                           ← XGI: coordinator, placement, router, scheduling
├── tests/                             ← unit tests, mock llama.cpp binaries, e2e/run-e2e.ps1
├── examples/cluster.yaml              ← example config (option D)
└── scripts/install-xi-task.ps1        ← registers xi.exe as a startup task
```

## Sources of truth

When documents disagree, this order wins:

1. **ADRs** (`adr/`): *why* decisions were made
2. **`architecture/05-protocols-and-data.md`**: every API, port, and config contract
3. **`architecture/02`–`04`**: component behaviour
4. **`distributed-qwen-cluster.md`**: the operator view. It must follow 1–3.

If you change a contract (an endpoint, field, port, or config key), update `05-protocols-and-data.md` **first**. Then update every doc that mentions it (search for the field name), including the operator guide.

## Design invariants: don't violate these without a new ADR

- **Windows-native.** Nothing Linux-only (vLLM, Ray, NCCL, WSL as a requirement).
- **CPU-only.** Don't assume GPU offload.
- **No tensor parallelism over Wi-Fi.** Use pipeline groups only to make a model fit, and replicas for speed ([ADR-002](adr/ADR-002-pipeline-not-tensor-parallelism.md)). Never suggest "add more nodes to a group to make it faster".
- **Groups are 1 head + (k−1) workers.** The head runs `llama-server` and must already hold the GGUF file. Workers run `rpc-server -c`.
- **`model_file` is relative to the agent's `--models-dir`** ([ADR-005](adr/ADR-005-model-files-relative-to-models-dir.md)).
- **All control-plane calls carry `X-Cluster-Token`** ([ADR-004](adr/ADR-004-shared-cluster-token.md)). llama.cpp ports (8080, 50052) have no auth and are protected by the firewall.
- **The coordinator keeps state in memory**, and agents re-register after a 404 ([ADR-006](adr/ADR-006-in-memory-coordinator-state.md)).
- **The same llama.cpp build runs on every node.**

## Key numbers (keep consistent across docs)

| Item | Value |
|---|---|
| Ports | router 8000 · coordinator 8100 · agent 8200 · llama-server 8080 · rpc-server 50052 |
| Usable RAM per node for weights | ~6.5 GB (of 8 GB free) |
| Qwen2.5-72B Q4_K_M | ~47 GB → 8 nodes/group → ~0.3–0.6 tok/s |
| Qwen3-30B-A3B Q4_K_M | ~18.6 GB → 3 nodes/group → ~5–10 tok/s (default pool) |
| Qwen2.5-7B Q4_K_M | ~4.7 GB → 1 node → ~5–8 tok/s |
| Heartbeat / node timeout | 5 s / 30 s |

Performance figures are **estimates** and must always be labelled as such until real benchmarks exist. Use the `cluster-capacity-planning` skill to recompute them.

## Writing conventions

- Markdown, using GitHub-flavoured tables and fenced code blocks. Diagrams are ASCII inside fenced code blocks, so they render anywhere.
- Commands are **PowerShell** for Windows nodes. Use `curl.exe` or `Invoke-RestMethod`, and quote JSON correctly for PowerShell.
- Number the architecture docs (`NN-name.md`) and link between them with relative links.
- Sizes are in GB (decimal) in prose, and in bytes in API schemas. Times are in seconds.
- Keep the operator guide task-focused. Rationale belongs in ADRs, and exact contracts belong in `05-protocols-and-data.md`.
- Never commit secrets. Examples use `api_keys: []`, the token placeholder `""`, and private IPs `192.168.1.x`.

## When to write an ADR

Write one for any decision that changes an invariant above, adds a new component or external dependency, or changes a public contract in a way that isn't backward compatible. Use the template in [adr/README.md](adr/README.md), take the next number, and add it to the ADR index and the docs index. Don't edit accepted ADRs, except for their status line.

## Project skills

| Skill | Use it when |
|---|---|
| `cluster-capacity-planning` | Sizing a model or pool: nodes per group, number of groups, estimated tok/s |
| `add-model-pool` | Adding or changing a model pool (config plus every doc that must change) |
| `node-setup-runbook` | Writing or following the setup steps for a Windows node |
| `cluster-troubleshooting` | Diagnosing a failing node, group, or request |
| `write-adr` | Recording an architecture decision |

## Out of scope

Training or fine-tuning, GPU clusters, internet-facing deployments, and non-Qwen model families. Mention them only as future work.
