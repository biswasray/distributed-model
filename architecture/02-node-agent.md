# 02 · Node Agent

> Component spec. Runs on **every** node. Port **8200**.

## 1. Responsibilities

1. **Register** the node with the coordinator: identity, hardware, and the model files on disk.
2. Send a **heartbeat** every `--heartbeat-s` seconds (default 5) with free RAM and the state of its child processes.
3. **Supervise** at most two llama.cpp child processes:
   - `rpc` → `rpc-server` (worker role)
   - `head` → `llama-server` (head role)
4. Expose a small, **token-protected** control API that the coordinator calls.

The agent has **no scheduling logic**. It never decides on its own to start or stop inference. It only does what the coordinator tells it.

## 2. Command-line interface

| Flag | Default | Description |
|---|---|---|
| `--coordinator` | *(required)* | Coordinator base URL, e.g. `http://192.168.1.10:8100` |
| `--token` | env `QWEN_CLUSTER_TOKEN` | Shared cluster token |
| `--host` / `--port` | `0.0.0.0` / `8200` | Interface and port for the agent API |
| `--name` | hostname | `node_id`. It must be unique in the cluster. |
| `--advertise-ip` | auto | The IP other nodes use to reach this one. Auto-detected from the route to the coordinator. |
| `--llama-dir` | `.\llama.cpp` | Folder containing `rpc-server.exe` and `llama-server.exe` |
| `--models-dir` | `.\models` | Folder scanned (recursively) for `*.gguf` files |
| `--log-dir` | `.\logs` | Agent and child process logs |
| `--rpc-port` | `50052` | Port `rpc-server` listens on |
| `--head-port` | `8080` | Port `llama-server` listens on |
| `--rpc-server-bin` / `--llama-server-bin` | — | Override the binary paths. A `.py` path runs with the agent's Python interpreter, which lets tests use mock binaries. |
| `--heartbeat-s` | `5` | Heartbeat interval |

The agent reports its port overrides in its registration, so several agents can run on one machine during development.

## 3. Lifecycle

```
 start ──► register ──ok──► heartbeat loop ◄────────────┐
              │                 │                       │
              └── fail: retry ◄─┤ 404 (coordinator      │
                  every 5 s     │  forgot node) ────────┘ re-register
                                │
                                └─ on shutdown: stop all child processes
```

- If registration or a heartbeat fails (network error), the agent retries forever and re-registers on the next attempt.
- An **HTTP 404** reply to a heartbeat means the coordinator restarted and lost its state. The agent re-registers right away.
- Child processes are **not** stopped when the coordinator is unreachable. Inference keeps running while the control plane recovers.

## 4. Control API

All endpoints require `X-Cluster-Token` when a token is configured. Request and response schemas are in [05-protocols-and-data.md](05-protocols-and-data.md#3-agent-api).

| Method | Path | Effect |
|---|---|---|
| GET | `/status` | Full node facts: hardware, models, process states |
| POST | `/rpc/start` | (Re)start `rpc-server` |
| POST | `/head/start` | (Re)start `llama-server` with a model and a list of RPC endpoints |
| POST | `/stop` | Stop `rpc`, `head`, or `all` |
| GET | `/logs/{role}?lines=N` | The last N lines (up to 500) of a child process's output |

### 4.1 Command lines the agent builds

**Worker:**

```
rpc-server -H 0.0.0.0 -p <rpc_port> -t <threads> [-c]
```

**Head:**

```
llama-server -m <models_dir>/<model_file> --host 0.0.0.0 --port <head_port>
             -c <context × parallel> -np <parallel> -t <threads> -ngl <ngl>
             [--rpc ip1:port,ip2:port,...] [--tensor-split a,b,c] [extra_args...]
```

- `threads` defaults to the number of **physical** CPU cores.
- `-c` is multiplied by `parallel`, because llama-server splits its context evenly between slots. That keeps the context per request equal to the pool's `context`.
- `--rpc` is left out for single-node pools.

## 5. Process supervision rules

| Rule | Detail |
|---|---|
| One process per role | Starting a role that is already running stops the old process first (restart semantics). |
| Graceful stop | Terminate, wait up to 15 seconds, then kill. |
| Output capture | stdout and stderr are merged, appended to `logs\<role>.log`, and the last 500 lines are kept in memory. |
| Windows specifics | Children start with `CREATE_NEW_PROCESS_GROUP`, so console signals don't spread to the agent. |
| Exit detection | The heartbeat reports `state: exited` with `exit_code`. The **coordinator** decides what to do about it. |
| Agent shutdown | All children are stopped (both in the lifespan shutdown and in `atexit`). |

## 6. Safety constraints

- `model_file` is a path **relative to `--models-dir`**. The agent resolves it and refuses anything outside the models folder, which blocks path traversal ([ADR-005](../adr/ADR-005-model-files-relative-to-models-dir.md)).
- The agent only ever runs the two configured llama.cpp binaries.
- `extra_args` are passed straight to `llama-server`. They are trusted because the caller holds the cluster token ([06-security.md](06-security.md)).

## 7. Node facts reported

`node_id`, `hostname`, `ip`, `agent_port`, `rpc_port`, `head_port`, `ram_total`, `ram_available`, `cpu_physical`, `cpu_logical`, `os`, `models[]`, `processes{rpc, head}`.

## 8. Running as a service

Register the agent as a Task Scheduler task that runs **at startup as SYSTEM**. The command is in the [setup guide §7.6](../distributed-qwen-cluster.md#76-run-the-node-agent-as-a-startup-service).

Also on each node: turn off sleep and hibernation, and set the Wi-Fi adapter power mode to *Maximum Performance*.
