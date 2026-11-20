# 05 · Protocols & Data

> The contracts between components: ports, headers, JSON schemas, and the config schema.

## 1. Network ports

| Port | Protocol | Listener | Callers | Auth |
|---|---|---|---|---|
| 8000 | HTTP | Router | Clients | Optional `Authorization: Bearer` |
| 8100 | HTTP | Coordinator (and the web console at `/ui/`) | Agents, router, operators, browsers | `X-Cluster-Token` (except `/health`, `/` and `/ui/*`) |
| 8200 | HTTP | Node Agent (and its web page at `/ui/`) | Coordinator, browsers | `X-Cluster-Token` (except `/`, `/ui/*`) |
| 8080 | HTTP | `llama-server` (head) | Router, coordinator (health) | **None**. Restrict with the firewall. |
| 50052 | TCP (llama.cpp RPC) | `rpc-server` (worker) | That group's head | **None**. Restrict with the firewall. |

Model downloads ([ADR-011](../adr/ADR-011-model-downloads-on-nodes.md)) need **outbound HTTPS (443)** from the nodes to `huggingface.co` and its CDN, or to `github.com`, `api.github.com` and `objects.githubusercontent.com`. XGI itself needs no internet access.

LAN copies ([ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md)) use the ports above and need no new firewall rule: a node pulls a file from another agent on **8200**, or from XGI on **8100**, with `X-Cluster-Token`.

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
  "model_sizes": {"Qwen3-30B-A3B-Q4_K_M.gguf": 18556686272},
  "processes": { "rpc": {"state": "stopped"}, "head": {"state": "stopped"} },
  "binaries": { "rpc_server": true, "llama_server": true },
  "runtimes": [
    {"language": "python", "label": "Python", "version": "Python 3.12.10", "path": "C:\\Python312\\python.exe"},
    {"language": "powershell", "label": "Windows PowerShell", "version": "5.1.26100.1", "path": "C:\\WINDOWS\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"}
  ],
  "exec_enabled": false,
  "downloads": {"available": true, "tool": "curl 8.13.0 (Windows) libcurl/8.13.0 Schannel ..."},
  "agent_version": "0.2.0"
}
```

`agent_version`, `arch`, `cpu_model`, `disk_free` (bytes free on the models dir's drive, or `null`), `model_sizes`, `binaries`, `runtimes`, `exec_enabled` and `downloads` are optional and informational: the console shows them ([ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md), [ADR-011](../adr/ADR-011-model-downloads-on-nodes.md)). `downloads.available` is false when the agent found no `curl` on its `PATH`. `model_sizes` maps each entry of `models` to its size in bytes. An agent that sends it also serves those files to other nodes (§3, "Model files"), so the coordinator uses it to pick LAN copy sources ([ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md)). The coordinator stores the whole body and returns it from `GET /nodes`, and ignores fields it doesn't know.

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
 "router_url": "http://127.0.0.1:8000", "exec_timeout_s": 60, "node_timeout_s": 30,
 "local_downloads": {"node_id": "@xgi", "available": true, "tool": "curl 8.13.0 ...", "models_dir": "C:\\qwen-cluster\\models",
                     "disk_free": 214748364800, "models": ["qwen2.5-7b-instruct-q4_k_m.gguf"]}}
```

`local_downloads` describes downloads onto the XGI machine itself (see "Model downloads" below).

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

### Model downloads ([ADR-011](../adr/ADR-011-model-downloads-on-nodes.md))

The console's **Models** tab and `xgi download` use these. They forward to the agents' download API (§3, "Model downloads") and need `X-Cluster-Token`. Like the rest of `/console/*`, they exist only while `console.enabled` is true.

The reserved node id **`@xgi`** means the XGI machine itself: XGI runs the same download manager as an agent (with its own `curl`), saving into `console.models_dir`. It works with no agents registered. Encode it as `%40xgi` in a URL path or query.

#### `GET /console/hub/files?node_id=&source=&repo=&ref=&all=`

Asks the agent on `node_id` to list a repository (so it also shows whether that node can reach the hub). `source` is `huggingface` (or `hf`) or `github` (or `gh`), `repo` is `owner/name`, `ref` is a branch or commit for Hugging Face (default `main`) or a release tag for GitHub (default `latest`). Only `.gguf` files are listed unless `all=true`. The response is the agent's `GET /hub/files` body plus `node_id`.

#### `GET /console/downloads[?node_id=]`

```json
{"downloads": [{"node_id": "LAB-PC-07", "id": "1422550f", "state": "downloading", "...": "the agent's job fields"}],
 "errors": [{"node_id": "LAB-PC-12", "error": "Connection timed out"}]}
```

The jobs of the XGI machine (`node_id: "@xgi"`) and of every alive node that can download (or of one node), newest first. The response also has `local`, the same object as `local_downloads` in `/console/info`. The coordinator asks the agents in parallel (4 s timeout each); a node that can't be read is listed in `errors`.

#### `POST /console/downloads`

```json
{"node_ids": ["LAB-PC-07", "LAB-PC-08"], "source": "huggingface", "repo": "Qwen/Qwen2.5-7B-Instruct-GGUF",
 "ref": "main", "files": ["qwen2.5-7b-instruct-q4_k_m.gguf"], "dest_dir": "qwen2.5-7b"}
```

Starts the same download on each node (`node_id` with one string also works). Everything except `node_ids` is sent to the agents as the `POST /downloads` body. Response `200` if at least one node started: `{"started": [<job + node_id>, ...], "errors": [{"node_id", "code", "error"}, ...]}`. If none started, the status is the first error's code and the body also has `error`.

**LAN copy** ([ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md)):

```json
{"node_ids": ["LAB-PC-08", "LAB-PC-09"], "source": "peer", "peer_node": "auto",
 "files": ["qwen3/Qwen3-30B-A3B-Q4_K_M.gguf"], "dest_dir": ""}
```

`source` is `peer` (or `lan`). `files` are paths in the source's models folder. `peer_node` is where the copies come from:

| `peer_node` | Source |
|---|---|
| a node id | That node, which must be alive. The coordinator sends its `http://<ip>:<agent_port>` as `peer_url`. |
| `@xgi` | The XGI machine's `console.models_dir`. Agents get `peer_url: "@xgi"` and use their own coordinator URL. |
| `auto` (default) | For each target, a source that has every file and isn't the target. First alive nodes not in a group, then the XGI machine, then nodes in a group, spread round robin. Only nodes that report `model_sizes` count. |

A target with no possible source, or that would copy from itself, is listed in `errors` (`404` / `400`). Each started job has `peer`, the source's node id (or `@xgi`).

#### `GET /console/peer/files`

```json
{"files": [{"path": "qwen3/Qwen3-30B-A3B-Q4_K_M.gguf", "size_bytes": 18556686272, "holders": ["@xgi", "LAB-PC-07"]}]}
```

Every `.gguf` file that can be copied over the LAN, sorted by path, with the sources that have it: `@xgi` (from `console.models_dir`) and the alive nodes that report `model_sizes`.

#### `GET /models` · `GET /models/file/<path>`

XGI serves `console.models_dir` to agents that copy from `@xgi`, the same way an agent serves its models folder (§3, "Model files"), with at most **4** files sending at once. They need `X-Cluster-Token` and exist only while `console.enabled` is true.

#### `POST /console/downloads/{node_id}/{id}/pause|resume|cancel` · `DELETE /console/downloads/{node_id}/{id}`

Forwarded to the agent; the response is the agent's (plus `node_id`). `404` unknown node · `409` the node is dead · otherwise the agent's status.

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

### Agent web page ([ADR-010](../adr/ADR-010-agent-web-page-and-runtime-settings.md))

`GET /` redirects to `/ui/`. `GET /ui/{index.html, app.css, xi.css, xi.js}` need **no token**. The calls below need `X-Cluster-Token`.

#### `GET /agent/state`

```json
{"version": "0.2.0", "started_at": 1762761600.0, "exec_jobs": 0,
 "node": { "...": "the same facts as GET /status" },
 "link": {"state": "registered", "coordinator": "http://192.168.1.10:8100", "node_id": "LAB-PC-07",
          "last_error": "", "registered_at": 1762761601.2, "last_heartbeat_ok": 1762761720.4}}
```

`link.state` is `not_configured` (no coordinator URL), `registering`, `registered` or `error` (see `last_error`).

#### `GET /agent/settings`

```json
{
  "path": "C:\\qwen-cluster\\xi.yaml", "file_exists": true,
  "configured": { "<xi.yaml keys>": "what the file says, with command-line flags applied" },
  "running":    { "<xi.yaml keys>": "what is in effect now" },
  "keys": ["coordinator", "name", "..."],
  "restart_keys": ["head_port", "host", "log_dir", "port", "rpc_port"],
  "restart_pending": ["rpc_port"],
  "cli_flags": {"log_dir": "--log-dir"},
  "token_set": true, "token_source": "env",
  "local_request": false,
  "defaults": {"name": "LAB-PC-07", "advertise_ip": "192.168.1.27", "rpc_server_bin": "llama.cpp\\rpc-server.exe", "llama_server_bin": "..."}
}
```

`token_source` is `flag`, `env`, `ui` (changed on the page, not saved) or `none`. `local_request` is true when the caller is on the node itself (loopback), the only place `allow_exec` can be turned on.

#### `PUT /agent/settings`

```json
{"settings": {"coordinator": "http://192.168.1.10:8100", "exec_max_timeout_s": 120}, "token": "optional new token"}
```

`settings` holds only the keys to change (§5.2). The whole file is validated and written atomically (the old one is kept as `xi.yaml.bak`). Every key applies at once except the restart keys. A live change makes the agent re-register. `token`, if not empty, replaces the token for the running process only and is **not** saved.

Response `200`: `{"ok": true, "path": "...", "live": ["coordinator"], "restart": ["rpc_port"], "warnings": ["..."]}`. `warnings` covers keys that are also command-line flags (the flag wins at the next start) and an unsaved token. Errors: `400` invalid setting · `403` `allow_exec: true` from a non-loopback address, while it is off · `500` the file can't be written.

#### `POST /agent/register` · `POST /agent/runtimes/detect`

`register` → `202`: re-register with the coordinator now. `runtimes/detect` → `200 {"runtimes": [...]}`: probe `PATH` for language runtimes again (takes a few seconds; `409` if one is already running), then re-register.

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

### Model downloads ([ADR-011](../adr/ADR-011-model-downloads-on-nodes.md))

The agent downloads files from a Hugging Face model repo or a GitHub release into its models folder with the system `curl`. Jobs are kept in memory (the 50 newest); a `.part` file survives an agent restart, and a new download of the same file resumes it.

#### `GET /hub/files?source=&repo=&ref=&all=`

```json
{"source": "huggingface", "repo": "Qwen/Qwen2.5-0.5B-Instruct-GGUF", "ref": "main", "total_files": 12,
 "files": [{"path": "qwen2.5-0.5b-instruct-q4_k_m.gguf", "size_bytes": 491400032}]}
```

Parameters as for `GET /console/hub/files`. For GitHub, `ref` comes back as the resolved tag (`latest` → `b4500`). `files` holds `.gguf` files only unless `all=true`; `total_files` counts all of them. Errors: `400` bad source, repo or ref · `404` repo, revision or release not found (Hugging Face also answers this way for private or gated repos without `HF_TOKEN`) · `502` the hub could not be reached or gave an error · `503` no `curl` on this node · `504` no answer within 60 s.

#### `POST /downloads`

```json
{"source": "huggingface", "repo": "Qwen/Qwen2.5-0.5B-Instruct-GGUF", "ref": "main",
 "files": ["qwen2.5-0.5b-instruct-q4_k_m.gguf"], "dest_dir": "qwen05"}
```

A LAN copy from another node or the XGI machine ([ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md)):

```json
{"source": "peer", "peer_url": "http://192.168.1.27:8200", "peer_name": "LAB-PC-07",
 "files": ["qwen3/Qwen3-30B-A3B-Q4_K_M.gguf"]}
```

| Field | Rules |
|---|---|
| `source` | `huggingface` \| `hf` \| `github` \| `gh` |
| `repo` | `owner/name`: each part 1–96 of `A-Z a-z 0-9 . _ -` |
| `ref` | Optional. Branch, tag or commit (1–128 of `A-Z a-z 0-9 . _ - + /`). Default `main` (Hugging Face) or `latest` (GitHub). |
| `files` | 1–64 paths in the repo (Hugging Face paths may contain `/`; GitHub asset names may not). No `.`/`..` segments, `\`, `:` or control characters. |
| `dest_dir` | Optional folder under the models folder, same rules. |
| `peer_url` | `source: "peer"` only. `"@xgi"` (the agent's own coordinator URL), or `http://<IPv4>:<port>` with an address in 10/8, 172.16/12, 192.168/16, 127/8 or 169.254/16. No hostnames, paths or HTTPS. |
| `peer_name` | Optional, `peer` only: the name shown in the job (default: `peer_url`). At most 128 printable characters. |

For `peer`, `repo` and `ref` are ignored, and `files` are `.gguf` paths in the peer's models folder (folders allowed).

Each file is saved as `<models_dir>/<dest_dir>/<file name>` (the repo's folders are dropped, so the parts of a split GGUF stay together), first as `<name>.part`. A **LAN copy keeps the whole path**: `<models_dir>/<dest_dir>/<path on the peer>`, so the pool's `model_file` is the same on the copy. The `model_file` to use in a pool is the `target` shown in the job.

Response `201`: the job (below), in state `starting`. Errors: `400` invalid request (for `peer`, also `@xgi` on a node with no coordinator URL) · `409` another unfinished job (including a paused one) writes the same file · `429` 3 downloads already running on this node · `503` no `curl`.

**Job:**

```json
{"id": "1422550f", "source": "huggingface", "repo": "Qwen/Qwen2.5-0.5B-Instruct-GGUF", "ref": "main",
 "dest_dir": "qwen05", "peer": null, "peer_url": null, "state": "downloading", "error": "", "note": "",
 "files": [{"path": "qwen2.5-0.5b-instruct-q4_k_m.gguf", "target": "qwen05/qwen2.5-0.5b-instruct-q4_k_m.gguf",
            "size_bytes": 491400032, "done_bytes": 159436800, "state": "downloading"}],
 "current_file": 0, "done_bytes": 159436800, "total_bytes": 491400032, "speed_bps": 16900000.0, "eta_s": 19.6,
 "created_at": 1794908930.9, "updated_at": 1794908940.1, "finished_at": null}
```

| `state` | Meaning |
|---|---|
| `starting` | Listing the repo: resolving the ref and file sizes, checking the files exist and that there is disk space |
| `downloading` | curl is running (`speed_bps` and `eta_s` are set) |
| `pausing` / `canceling` | Stop requested; becomes `paused` / `canceled` within a second |
| `paused` | Stopped; the `.part` files are kept |
| `completed` | Every file is in place. A file that was already there (same size) has file state `present` and is not downloaded again. |
| `failed` | See `error`. Resume retries it from where it stopped. |
| `canceled` | Stopped and the `.part` files deleted |

A LAN copy has `source: "peer"`, `repo` and `ref` empty, `peer` (its `peer_name`) and `peer_url` (resolved, so `@xgi` becomes the coordinator's origin). `note` says why a running job is waiting, for example `waiting: LAB-PC-07 is busy sending other copies`, and is otherwise empty.

File `state` is `pending`, `downloading`, `done` or `present`. `total_bytes` and `size_bytes` are `null` if the hub gave no size. A failed transfer is retried 5 times (3 s, 6 s, …), and a stall (under 1 byte/s for 2 minutes) counts as a failure. HTTP 401/403/404 fail at once. For a LAN copy, `429` or `503` from the peer is not a failure: the job sets `note` and tries again every 15 s until the peer has a free slot, or the job is paused or canceled. curl doesn't follow redirects from a peer. A file that would overwrite a different-sized existing file fails. After each completed file the agent re-registers, so the coordinator's `models` list includes it.

#### `GET /downloads`

`{"downloads": [<job>, ...], "available": true}`, newest first.

#### `POST /downloads/{id}/pause` · `/resume` · `/cancel` · `DELETE /downloads/{id}`

`pause` (running jobs) and `cancel` (anything not completed) return the job. `resume` takes a `paused` or `failed` job and returns it. `DELETE` removes a job that isn't running from the list (its `.part` files stay, so a paused download can still be resumed by starting it again). Errors: `404` unknown id · `409` wrong state (e.g. resume while still stopping, cancel after completion) · `429` (resume) 3 downloads running.

#### Model files (LAN copies, [ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md))

`GET /models` → `{"files": [{"path": "qwen3/Qwen3-30B-A3B-Q4_K_M.gguf", "size_bytes": 18556686272}]}`: the `.gguf` files in the models folder, sorted by path.

`GET /models/file/<path>` sends one of them (`application/octet-stream`), honouring `Range` (`206`), so a receiver's `curl -C -` resumes. `<path>` is percent-encoded per segment. Only complete `.gguf` files that resolve inside the models folder (after symlinks) are served. Errors: `400` not a `.gguf` path inside the folder · `404` no such file · `429` already sending **2** files (`Retry-After: 20`). Every transfer is logged with the receiver's address. Both endpoints need `X-Cluster-Token`, like the rest of the agent API.

Gated or private repos need `HF_TOKEN` (or `HUGGING_FACE_HUB_TOKEN`) for Hugging Face and `GITHUB_TOKEN` (or `GH_TOKEN`) for GitHub **in the agent's environment**. curl gets the token in a temporary header file, never on its command line. The same goes for the cluster token on a LAN copy. For tests, `XI_HF_BASE`, `XI_GITHUB_BASE` and `XI_GITHUB_API_BASE` replace the hub origins.

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
  models_dir: models         # where downloads onto the XGI machine go (node "@xgi"); relative = XGI's working folder

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

### 5.2 `xi.yaml` (node agent settings)

Read by `xi` at startup: `--config PATH`, else `./xi.yaml`, else `xi.yaml` next to the executable. A missing file means defaults (the first save from the agent page creates it). **Precedence: built-in defaults < `xi.yaml` < command-line flags.** Every key has the flag `--<key with - for _>`.

```yaml
coordinator: "http://192.168.1.10:8100"   # "" = not configured: the agent waits
name: ""                     # node_id; "" = hostname
advertise_ip: ""             # "" = the route to the coordinator
heartbeat_s: 5
llama_dir: "llama.cpp"
rpc_server_bin: ""           # "" = <llama_dir>/rpc-server[.exe]
llama_server_bin: ""         # "" = <llama_dir>/llama-server[.exe]
models_dir: "models"
allow_exec: false            # can only be turned ON locally (ADR-010)
exec_dir: "exec"
exec_max_timeout_s: 300
host: "0.0.0.0"              # restart keys from here down
port: 8200
log_dir: "logs"
rpc_port: 50052
head_port: 8080
```

| Rule | Error |
|---|---|
| Unknown key, or `token` / `cluster_token` (the token is never stored here) | Startup fails |
| `coordinator` is empty or an `http://host:port` URL | Startup fails |
| Ports 1–65535 and all different; `heartbeat_s`, `exec_max_timeout_s` > 0; folders not empty | Startup fails |

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
