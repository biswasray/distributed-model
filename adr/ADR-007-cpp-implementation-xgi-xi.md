# ADR-007: Implement the control plane in C++ as XGI and XI

- **Status:** Accepted
- **Date:** 2026-11-13

## Context

The specs ([02](../architecture/02-node-agent.md)–[05](../architecture/05-protocols-and-data.md)) were written with a Python/FastAPI implementation in mind. Python means installing an interpreter, a virtualenv and pip packages on all 50 PCs, and running `pip install` over shared Wi-Fi. The operators want fewer moving parts per node. The Windows-native invariant still applies, and llama.cpp stays the inference engine ([ADR-001](ADR-001-llama-cpp-as-inference-engine.md)).

## Decision

Implement the control plane in **C++20**, built with **CMake and MSVC**, as two executables:

| Executable | Name | Contains | Ports |
|---|---|---|---|
| `xgi.exe` | **XGI**, Extended General Intelligence: the cluster core | Coordinator (`xgi coordinator`), router (`xgi router`), or both in one process (`xgi serve`) | 8100, 8000 |
| `xi.exe` | **XI**, Extended Interface: the node agent | Agent API and llama.cpp process supervisor | 8200 |

- Inference stays in llama.cpp (`llama-server`, `rpc-server`). XGI orchestrates it. It is **not** a new inference engine.
- Every wire contract in [05-protocols-and-data.md](../architecture/05-protocols-and-data.md) is unchanged, including `QWEN_CLUSTER_TOKEN` and `X-Cluster-Token`.
- Binaries link the **static CRT** (`/MT`), so they can be copied to a node with no runtime install.
- Third-party libraries are fetched at configure time by CMake `FetchContent`, **pinned by version and SHA-256** in `cmake/Dependencies.cmake`:
  - [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.18.3: HTTP server and client, header-only, no TLS (LAN only)
  - [nlohmann/json](https://github.com/nlohmann/json) 3.11.3: JSON
  - [yaml-cpp](https://github.com/jbeder/yaml-cpp) 0.8.0: `cluster.yaml`

## Alternatives considered

| Option | Why not |
|---|---|
| Python + FastAPI (as originally sketched) | An interpreter and pip packages on every node. More to install, patch, and keep identical. |
| Go | A single static binary too, but the team asked for C++ and MSVC tooling |
| Boost.Beast / Asio | Heavier dependency and build. cpp-httplib covers our needs (thread-per-request is fine at this scale). |
| Hand-written HTTP/JSON | More code to get wrong, no benefit |

## Consequences

- ✅ One `xi.exe` per node, no runtime. Upgrading is a file copy.
- ✅ Job objects (kill-on-close) guarantee llama.cpp children die with the agent, even if it crashes.
- ✅ The placement and routing logic is pure and unit-tested. An e2e test runs the whole control plane on one PC with mock llama.cpp binaries.
- ❌ Building needs Visual Studio 2022+ (or Build Tools) and internet access, or pre-downloaded archives, for the first configure.
- ❌ cpp-httplib uses a thread per connection. The router sizes its pool to `slots + pools × max_queue`. That's fine for ~50 nodes, but it's not meant for thousands of concurrent clients.
- ❌ `--rpc-server-bin` / `--llama-server-bin` paths ending in `.py` run with `python` from `PATH` (there is no "agent interpreter" any more). Tests use the C++ mock binaries instead.
- 🔜 Code-sign `xgi.exe` / `xi.exe`, so Windows Smart App Control and WDAC allow them without reputation delays.
