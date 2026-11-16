# 05 · Protocols & Data

> The contracts between components: ports, headers, JSON schemas, and the config schema.

## 1. Network ports

| Port | Protocol | Listener | Callers | Auth |
|---|---|---|---|---|
| 8000 | HTTP | Router | Clients | Optional `Authorization: Bearer` |
| 8100 | HTTP | Coordinator (and the web console at `/ui/`) | Agents, router, operators, browsers | `X-Cluster-Token` (except `/health`, `/` and `/ui/*`) |
| 8200 | HTTP | Node Agent | Coordinator | `X-Cluster-Token` |
| 8080 | HTTP | `llama-server` (head) | Router, coordinator (health) | **None**. Restrict with the firewall. |
| 50052 | TCP (llama.cpp RPC) | `rpc-server` (worker) | That group's head | **None**. Restrict with the firewall. |

All control-plane bodies are JSON (`Content-Type: application/json`). Sizes are in **bytes** and times in **seconds** (as floats).

Errors from every component (router, coordinator, agent) use the same body: `{"error": {"message": ..., "type": ..., "code": ...}}` (see §4). A missing or wrong `X-Cluster-Token` gets `401`.

## 2. Coordinator API

### `POST /register` (agent → coordinator)

```json
{
  "node_id": "LAB-PC-07",
  "hostname": "LAB-PC-07",
  "ip": "192.168.1.27",
  "agent_port": 8200,
  "rpc_port": 50052,
  "head_port": 8080,
  "ram_total": 17062440960,
  "ram_available": 8589934592,
  "cpu_physical": 6,
  "cpu_logical": 12,
  "os": "Windows-11-10.0.26200-SP0",
  "arch": "x86_64",
  "cpu_model": "Intel(R) Core(TM) i5-8500 CPU @ 3.00GHz",
  "disk_free": 214748364800,
  "models": ["Qwen3-30B-A3B-Q4_K_M.gguf"],
  "processes": { "rpc": {"state": "stopped"}, "head": {"state": "stopped"} },
  "binaries": { "rpc_server": true, "llama_server": true },
  "runtimes": [
    {"language": "python", "label": "Python", "version": "Python 3.12.10", "path": "C:\\Python312\\python.exe"},
    {"language": "powershell", "label": "Windows PowerShell", "version": "5.1.26100.1", "path": "C:\\WINDOWS\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"}
  ],
  "exec_enabled": false,
  "agent_version": "0.2.0"
}
```

`agent_version`, `arch`, `cpu_model`, `disk_free` (bytes free on the models dir's drive, or `null`), `binaries`, `runtimes` and `exec_enabled` are optional and informational: the console shows them ([ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md)). The coordinator stores the whole body and returns it from `GET /nodes`, and ignores fields it doesn't know.

`runtimes[].language` is one of `python`, `javascript`, `powershell` (Windows only), `pwsh`, `bash`, `sh` (POSIX only), `cmd` (Windows only), `ruby`, `perl`, `php`, `lua`, `go`, `java`, `c`, `cpp`, `rust`. Only runtimes found on the agent's `PATH` whose version probe succeeds are listed.

Response `200`: `{"ok": true, "heartbeat_interval_s": 5}`

### `POST /heartbeat` (agent → coordinator)

```json
{
  "node_id": "LAB-PC-07",
  "ram_available": 1610612736,
  "processes": {
    "rpc":  {"state": "running", "pid": 4120, "exit_code": null, "uptime_s": 812.4},
    "head": {"state": "stopped"}
  }
}
```

Responses: `200 {"ok": true}` · `404` for an unknown node, or a node already marked `dead` (the agent must re-register).

Process `state` is `stopped` (never started, or stopped on request), `running`, or `exited` (it ended without being asked to). `pid` stays set for `exited`, so the coordinator can tell an old exit from the process it just started.

### `GET /groups` (router, operator → coordinator)

```json
[
  {
    "group_id": "qwen3-30b-a3b-0",
    "pool": "qwen3-30b-a3b",
    "status": "healthy",
    "head": "LAB-PC-01",
    "workers": ["LAB-PC-14", "LAB-PC-15"],
    "head_url": "http://192.168.1.21:8080",
    "slots": 1,
    "error": null,
    "started_at": 1762761600.0,
    "healthy_at": 1762761733.5
  }
]
```

### `GET /nodes`

A list of node records ([03-coordinator.md §2.1](03-coordinator.md#21-node-record)), plus `status`, `draining`, `group_id`, `role`, and `last_seen`.

### Operator actions

| Call | Response |
|---|---|
| `POST /deploy[?force=true]` | `200 {"groups": [...]}` · `409` if already deployed |
| `POST /undeploy` | `200 {"stopped": N}` |
| `POST /groups/{id}/restart` | `202 {"group_id": ..., "status": "starting"}` · `404` |
| `POST /drain/{node_id}` · `/undrain/{node_id}` | `200 {node record}` · `404` |

### Web console ([ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md))

Served on the coordinator port when `console.enabled` is true. `GET /` redirects to `/ui/`. `GET /ui/` and `GET /ui/{index.html, app.css, app.js}` are static files and need **no token**. The `/console/*` calls need `X-Cluster-Token`.

#### `GET /console/info`

```json
{"version": "0.2.0", "allow_exec": false, "model": "qwen3-30b-a3b", "pools": ["qwen3-30b-a3b"],
 "router_url": "http://127.0.0.1:8000", "exec_timeout_s": 60, "node_timeout_s": 30}
```

#### `POST /console/generate`

Asks pool `console.model` (through the router's `/v1/chat/completions`, non-streaming, `temperature` 0.2, `max_tokens` = `console.max_tokens`) for one program.

```json
{
  "node_id": "LAB-PC-07",
  "language": "python",
  "prompt": "List the 5 largest files in the current folder",
  "history": [{"role": "user", "content": "..."}, {"role": "assistant", "content": "```python\n...\n```"}]
}
```

`node_id` and `history` are optional. The node's `os`, `arch` and runtime version go into the system prompt. Only the last 12 `user`/`assistant` turns are used. Limits: `prompt` ≤ 16 KB, `history` ≤ 64 KB.

Response `200`:

```json
{"language": "python", "code": "import os\n...", "reply": "```python\n...```", "model": "qwen3-30b-a3b",
 "group": "qwen3-30b-a3b-2", "finish_reason": "stop", "duration_s": 41.7}
```

`code` is the first fenced block of `reply`, with any `<think>…</think>` removed. Errors: `400` bad input · `404` unknown node · `502` router unreachable or bad reply · router `429`/`503` passed through.

#### `POST /console/exec`

```json
{"node_id": "LAB-PC-07", "language": "python", "code": "print('hi')", "timeout_s": 60}
```

`timeout_s` defaults to `console.exec_timeout_s`. The coordinator forwards the call to the node's `POST /exec` (§3) and adds `node_id` to the agent's response. Errors: `403` if `console.allow_exec` is false or the node has `exec_enabled: false` · `404` unknown node · `409` dead node · `502` agent unreachable · agent `400`/`429` passed through.

#### `GET /console/config`

The config file and the settings XGI is running with (§5.1).

```json
{
  "path": "C:\\qwen-cluster\\cluster.yaml",
  "file_exists": true,
  "yaml": "<the file's text, or a generated default if it doesn't exist>",
  "config": { "coordinator": {...}, "router": {...}, "llama_cpp": {...}, "console": {...}, "pools": [...], "spares": 2, "pinned": [] },
  "running": { "...": "same shape: what is in effect now" },
  "token_set": true,
  "token_from_env": true,
  "restart_pending": ["router.port"],
  "parse_error": null
}
```

`config` and `running` use the `cluster.yaml` keys **without `cluster_token`**. `restart_pending` lists settings that differ between the file and the running process and only apply after a restart. `parse_error` is set when the file on disk is invalid; `config` then shows the running settings.

#### `POST /console/config/check` · `PUT /console/config`

Validate (check) or validate, write and apply (put). The body is one of:

| Body | Meaning |
|---|---|
| `{"yaml": "<text>"}` | The new file, written byte-for-byte (comments kept) |
| `{"config": {<cluster.yaml keys>}}` | The form's settings. They are rendered to YAML in a standard layout (comments dropped), keeping the file's own `cluster_token`. |

Response `200`:

```json
{"ok": true, "saved": true, "path": "C:\\qwen-cluster\\cluster.yaml", "yaml": "...",
 "live": ["pools", "spares", "console.max_tokens"], "restart": ["router.port"], "warnings": []}
```

`live` = changed settings that took effect at once; `restart` = changed settings that apply on the next XGI start. Errors: `400 {"ok": false, "error": "...", ...}` if the config is invalid (the same rules as at startup, §5) · `500` if the file can't be written. On save, the previous file is kept as `cluster.yaml.bak`, and the new one replaces it atomically.

## 3. Agent API

### `POST /rpc/start`

```json
{ "threads": null, "cache": true }
```

`threads: null` means use the number of physical cores.

### `POST /head/start`

```json
{
  "model_file": "Qwen3-30B-A3B-Q4_K_M.gguf",
  "rpc_endpoints": ["192.168.1.34:50052", "192.168.1.35:50052"],
  "context": 4096,
  "parallel": 1,
  "threads": null,
  "ngl": 99,
  "tensor_split": null,
  "extra_args": []
}
```

Errors: `400` if `model_file` is missing or outside the models dir.

Both start calls return the process info:

```json
{"state": "running", "pid": 5230, "exit_code": null, "uptime_s": 0.0}
```

### `POST /stop`

`{"role": "rpc" | "head" | "all"}` → the info for both roles.

### `GET /status`

The full node facts, with the same shape as the `/register` body.

### `GET /logs/{role}?lines=100`

```json
{"role": "head", "lines": ["...", "..."]}
```

### `POST /exec`

Runs a program with one of the node's detected `runtimes` ([ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md)). Only available when the agent runs with `--allow-exec`.

```json
{"language": "python", "code": "print('hi')", "timeout_s": 60}
```

`code` is 1 byte to 256 KB. `timeout_s` (default 60) is capped at `--exec-max-timeout-s` and covers the compile step too (C, C++, Rust). The program runs with `--exec-dir` as its working directory and empty stdin.

Response `200`, also when the program fails or times out:

```json
{"language": "python", "runtime": "Python 3.12.10", "stage": "run", "exit_code": 0, "timed_out": false,
 "duration_s": 0.42, "output": ["hi"], "output_truncated": false}
```

`output` is the merged stdout and stderr, as the last 500 lines, each cut at 4000 characters. `stage` is `compile` if a compile step failed. `exit_code` is `null` when the program was killed for timing out. Errors: `403` exec disabled · `400` bad input, or the language isn't available on this node · `429` 2 jobs already running (`Retry-After: 5`).

## 4. Router API

A subset of the OpenAI API. Any OpenAI SDK works with `base_url=http://<router>:8000/v1`.

### `POST /v1/chat/completions`

| Field | Type | Required | Notes |
|---|---|---|---|
| `model` | string | ✅ | Pool name |
| `messages` | array | ✅ | `[{role, content}]` |
| `stream` | bool | | Recommended for the slow pools |
| `max_tokens` | int | | Strongly recommended on `qwen-72b` |
| `temperature`, `top_p`, `stop`, `seed` | | | Passed through to llama-server |

### Error format

```json
{"error": {"message": "queue full", "type": "capacity", "code": 429}}
```

| Code | When |
|---|---|
| 401 | Missing or invalid client API key |
| 429 | The pool's queue is full (`Retry-After` header set) |
| 502 | The head failed and the retry failed too |
| 503 | Unknown model, no healthy group, or queue timeout |

## 5. Configuration schema

`cluster.yaml` lives on the control machine and is read by the coordinator and the router.

```yaml
cluster_token: ""            # better: set env QWEN_CLUSTER_TOKEN on every machine

coordinator:
  host: 0.0.0.0
  port: 8100
  heartbeat_interval_s: 5
  node_timeout_s: 30
  group_start_timeout_s: 7200

router:
  host: 0.0.0.0
  port: 8000
  coordinator_url: http://127.0.0.1:8100
  max_queue: 100
  queue_timeout_s: 600
  request_timeout_s: 1800
  slots_per_group: 1
  api_keys: []

llama_cpp:
  rpc_cache: true            # pass -c to rpc-server
  extra_head_args: []        # appended to every llama-server

console:                     # web console at http://<coordinator>:8100/ui/ (ADR-009)
  enabled: true
  allow_exec: false          # allow POST /console/exec; nodes also need xi --allow-exec
  router_url: http://127.0.0.1:8000   # code generation goes through the router
  model: ""                  # pool that writes code; "" = the first pool
  max_tokens: 1024
  generate_timeout_s: 600
  exec_timeout_s: 60         # default run timeout offered by the UI

pools:                       # filled in this order
  - name: qwen3-30b-a3b      # = the client "model" field
    model_file: Qwen3-30B-A3B-Q4_K_M.gguf   # RELATIVE to each head's models dir
    nodes_per_group: 3
    groups: 10
    context: 4096
    parallel: 1              # slots per group (optional)
    tensor_split: null       # e.g. [1, 1, 1] (optional)
    extra_args: []           # optional

spares: 2                    # nodes kept free for failover

pinned:                      # optional: prefer these nodes as heads for a pool
  - host: LAB-PC-01
    role: head
    pool: qwen-72b
```

### Validation rules

| Rule | Error |
|---|---|
| Each pool has `name`, `model_file`, `nodes_per_group`, `groups` | Startup fails |
| `name` is unique | Startup fails |
| `nodes_per_group ≥ 1`, `groups ≥ 0` | Startup fails |
| `model_file` is a relative path ending in `.gguf` (the first part, for split files) | Startup fails |
| Σ(`nodes_per_group × groups`) + `spares` > live nodes | **Warning**. The extra groups stay `unplaced`. |
| `console.max_tokens ≥ 1`, `console.*_timeout_s > 0` | Startup fails |
| `console.model` is not a pool name | **Warning**. Code generation fails. |

### Precedence

Environment `QWEN_CLUSTER_TOKEN` › `cluster.yaml` › built-in defaults.

The console calls the router with the first entry of `router.api_keys` (if any), so the coordinator and router must read the same `cluster.yaml`.

### 5.1 Where XGI finds `cluster.yaml`, and changing it while running

| `--config` | Behaviour |
|---|---|
| Given | That file must exist and be valid, otherwise startup fails. |
| Not given | `./cluster.yaml` (the current folder), then `cluster.yaml` **next to the `xgi` executable**. If neither exists, XGI starts with the built-in defaults above and **no pools**, and logs a warning. A save from the console then creates `cluster.yaml` next to `xgi`. |

A save from the console (`PUT /console/config`) applies these sections at once, without a restart:

| Applies at once | Applies after XGI restarts |
|---|---|
| `pools`, `spares`, `pinned`, `llama_cpp`, `console` (except `enabled`) | `cluster_token`, `coordinator.*`, `router.*`, `console.enabled` |

Pool changes update the coordinator's plan. Running groups keep their layout until the next `POST /deploy?force=true`. A router running as its own process (`xgi router`) reads the file only at startup.
