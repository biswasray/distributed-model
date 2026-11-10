# 05 · Protocols & Data

> The contracts between components: ports, headers, JSON schemas, and the config schema.

## 1. Network ports

| Port | Protocol | Listener | Callers | Auth |
|---|---|---|---|---|
| 8000 | HTTP | Router | Clients | Optional `Authorization: Bearer` |
| 8100 | HTTP | Coordinator | Agents, router, operators | `X-Cluster-Token` (except `/health`) |
| 8200 | HTTP | Node Agent | Coordinator | `X-Cluster-Token` |
| 8080 | HTTP | `llama-server` (head) | Router, coordinator (health) | **None**. Restrict with the firewall. |
| 50052 | TCP (llama.cpp RPC) | `rpc-server` (worker) | That group's head | **None**. Restrict with the firewall. |

All control-plane bodies are JSON (`Content-Type: application/json`). Sizes are in **bytes** and times in **seconds** (as floats).

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
  "models": ["Qwen3-30B-A3B-Q4_K_M.gguf"],
  "processes": { "rpc": {"state": "stopped"}, "head": {"state": "stopped"} }
}
```

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

Responses: `200 {"ok": true}` · `404` for an unknown node (the agent must re-register).

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

### Precedence

Environment `QWEN_CLUSTER_TOKEN` › `cluster.yaml` › built-in defaults.
