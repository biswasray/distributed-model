# 03 · Coordinator

> Component spec. A single instance on the control machine. Port **8100**. Implemented by **XGI** (`xgi coordinator` or `xgi serve`, [ADR-007](../adr/ADR-007-cpp-implementation-xgi-xi.md)).

## 1. Responsibilities

1. Keep the **node registry** up to date from registrations and heartbeats.
2. **Plan placement**: turn `cluster.yaml` pools into concrete groups of nodes.
3. Drive each group's **lifecycle**: start workers, start the head, wait for health, stop.
4. **Detect failures and repair** groups automatically.
5. Expose cluster state to the router and to operators.
6. Serve the **web console** (§10): a dashboard of nodes and groups, and a chat box that generates code with the cluster's model and runs it on a node.

## 2. State model

All state lives **in memory** ([ADR-006](../adr/ADR-006-in-memory-coordinator-state.md)).

### 2.1 Node record

| Field | Source | Notes |
|---|---|---|
| `node_id`, `ip`, `agent_port`, `rpc_port`, `head_port` | register | Identity and addresses |
| `ram_total`, `ram_available`, `cpu_physical` | register / heartbeat | Used by placement |
| `models[]` | register | GGUF files on the node's disk, relative to its models dir |
| `processes` | heartbeat | `{rpc, head}` → `stopped` / `running` / `exited` |
| `os`, `arch`, `cpu_model`, `disk_free`, `binaries`, `runtimes[]`, `exec_enabled` | register | Shown by the console. Not used by placement. |
| `last_seen` | heartbeat | Wall-clock time of the last heartbeat |
| `status` | derived | `alive` or `dead` (no heartbeat for `node_timeout_s`) |
| `draining` | operator | If true, the node is never picked for new placements |
| `group_id`, `role` | placement | `head` / `worker` / none (free) |

### 2.2 Group record

| Field | Notes |
|---|---|
| `group_id` | `<pool>-<index>`, e.g. `qwen3-30b-a3b-3` |
| `pool` | Pool name |
| `head` | `node_id` of the head |
| `workers[]` | `node_id`s **in pipeline order** |
| `status` | See §3 |
| `head_url` | `http://<head ip>:<head_port>` |
| `slots` | The pool's `parallel` value (concurrent requests per group) |
| `error` | Last failure reason, for operators. It stays set while the group restarts and clears when the group is `healthy`. |
| `started_at`, `healthy_at` | Timestamps |

## 3. Group state machine

```
             plan ok                  workers' RPC ports open       /health == 200
 unplaced ───────────► starting ───────────────────────────► loading ────────────► healthy
    ▲                     │                                      │                     │
    │                     │ RPC port timeout / agent error       │ head exited /       │ member dead,
    │                     ▼                                      │ load timeout        │ process exited,
    │                  failed ◄──────────────────────────────────┘                     │ 3× health fail
    │                     │                                                             ▼
    │                     └────────── repair ◄────────────────────────────────────  degraded
    │                                   │
    └──── no replacement available ─────┘
 (any state) ── undeploy ──► stopped
```

| Status | Router sends traffic? | Meaning |
|---|---|---|
| `unplaced` | No | Not enough eligible free nodes. Placement is retried every monitor cycle. |
| `starting` | No | Worker `rpc-server`s are starting |
| `loading` | No | `llama-server` is loading or sending weights. **It can take an hour on the first deployment.** |
| `healthy` | **Yes** | Serving |
| `degraded` | No | A failure was detected and repair is about to start |
| `failed` | No | Start failed. Repair or replacement is pending. |
| `stopped` | No | Undeployed by the operator |

## 4. Placement algorithm

The input is `cluster.yaml` (`pools`, `spares`, `pinned`) plus the alive, non-draining, unassigned nodes.

```
free = alive ∧ ¬draining ∧ unassigned
for pool in pools (config order):
    for i in 0 .. pool.groups-1:
        need = pool.nodes_per_group
        if |free| − need < spares:            → group unplaced ("would consume spares")
        heads = [n ∈ free | pool.model_file ∈ n.models]
        if heads = ∅:                          → group unplaced ("no free node has model file")
        head  = first of heads sorted by (pinned-for-this-pool first, ram_available desc)
        pick need−1 workers from free∖{head}, sorted by
              (holds a model file for any pool ? 1 : 0,  ram_available desc)
        if not enough workers:                 → group unplaced
        assign; free −= {head} ∪ workers
```

Design notes:

- **Heads must already hold the GGUF file.** The coordinator never copies model files ([ADR-005](../adr/ADR-005-model-files-relative-to-models-dir.md)).
- **Workers are chosen from nodes without model files first.** Nodes that hold model files stay available as heads.
- **Pools are filled in config order.** Put the most important pool first.
- Worker order is the pipeline order. It's stable for a given deployment, so each worker's tensor cache stays valid across restarts.
- *Future:* group nodes by access point to cut cross-AP hops (see [ADR-002](../adr/ADR-002-pipeline-not-tensor-parallelism.md)).

## 5. Group start sequence

```
1. status = starting
2. for each worker (in parallel):  POST agent /rpc/start {cache: llama_cpp.rpc_cache}
3. wait until TCP <worker ip>:<rpc_port> accepts connections   (timeout 60 s each)
4. status = loading
   POST head agent /head/start {model_file, rpc_endpoints=[ip:port in worker order],
                                context, parallel, ngl=99, tensor_split?, extra_args}
5. poll GET <head_url>/health every 5 s until 200
      - 503 = still loading → keep waiting (up to group_start_timeout_s, default 7200 s)
      - head process reported `exited` → failed
6. status = healthy
```

**Stopping a group** sends `POST /stop {role: all}` to every member, ignores errors, and frees the members.

A process failure is judged against the **pid the coordinator started**. An `exited` state with that pid counts. So does a heartbeat received more than 2 seconds after the start that shows the role not `running`, or running with a different pid (for example, after the agent restarted).

## 6. Failure handling

A monitor loop runs every `heartbeat_interval_s`:

| Check | Trigger | Action |
|---|---|---|
| Node liveness | `now − last_seen > node_timeout_s` | Mark the node `dead`. If it's in a group, mark the group `degraded`. |
| Process exit | Heartbeat shows the member's role process `exited` while the group is `loading` or `healthy` | Group `degraded` (or `failed` while loading) |
| Head health | `GET /health` fails 3 times in a row while `healthy` | Group `degraded` |
| Unplaced retry | A group is `unplaced` and enough free nodes exist | Place and start it |

**Repair procedure** (one task per group, never two at once):

1. Stop all *alive* members of the group.
2. Release the dead or failed members from the group. Release the survivors back to `free` as well, so placement can rebuild the group in the best order.
3. Re-run placement for this one group (spares are now usable).
4. Start it again (§5). If placement fails, the group becomes `unplaced`.

A node that comes back after being marked dead gets `404` on its next heartbeat, re-registers, and joins `free`. It doesn't automatically return to its old group.

**Repair backoff.** The first repair of a group starts immediately. If the group fails again before it reaches `healthy`, the next repair waits `heartbeat_interval_s × 2^(n−1)`, up to 300 seconds. This stops a broken model file or a crashing head from looping hot.

**Unplaced groups.** A group left `unplaced` by a failed repair may use the spares when it is retried. A group that was never placed keeps the spares free.

## 7. API

| Method | Path | Caller | Description |
|---|---|---|---|
| GET | `/health` | anyone | Liveness check. **No authentication.** |
| POST | `/register` | agent | Insert or replace a node record. The reply includes the heartbeat interval. |
| POST | `/heartbeat` | agent | Update liveness. Returns **404 for an unknown `node_id`**, which makes the agent re-register. |
| GET | `/nodes` | operator | All node records |
| GET | `/groups` | router, operator | All groups with status, `head_url`, `slots` |
| POST | `/deploy` | operator | Plan and start every pool. Returns 409 if already deployed, unless `?force=true` (which undeploys first). |
| POST | `/undeploy` | operator | Stop every group |
| POST | `/groups/{id}/restart` | operator | Stop and start one group with the same members, if they're all alive. Otherwise the group is repaired, which re-places it. |
| POST | `/drain/{node_id}` | operator | Exclude the node from placement. If it's in a group, the group is repaired without it. |
| POST | `/undrain/{node_id}` | operator | Make the node eligible again |
| GET | `/`, `/ui/…` | browser | Console page and its static files. **No authentication.** |
| GET | `/console/info` | console | Console settings |
| POST | `/console/generate` | console | Generate a program through the router |
| POST | `/console/exec` | console | Run a program on a node through its agent's `/exec` |
| GET | `/console/config` | console | The config file and the running settings |
| POST | `/console/config/check` | console | Validate a new config |
| PUT | `/console/config` | console | Validate, save and apply a new config (§10.1) |

Every endpoint except `/health` and the console's static files requires `X-Cluster-Token`. Schemas are in [05-protocols-and-data.md](05-protocols-and-data.md#2-coordinator-api).

## 8. Concurrency rules

- One mutex protects all registry and group state, including placement, so two repairs can never claim the same free node. It is never held during a network call.
- Each group has at most one lifecycle task (a thread). A new start or repair cancels the old one and **waits for it to finish**, so an old in-flight call can't land after the new task's `/stop`.
- Calls to agents use timeouts: 10 s in general, 30 s for `/rpc/start` and `/head/start` (a restart first stops the old process, with up to 15 s grace), and 40 s for `/stop`. Head health checks run in parallel with a 5 s timeout, so one unreachable node can't block the monitor loop.

## 9. Restart behaviour

When the coordinator restarts, it starts with no state. Agents get a 404 on their next heartbeat and re-register within about 5 seconds. Any llama.cpp processes still running are **not adopted**. The operator runs `POST /deploy` again, which restarts them. Workers reload from their tensor cache, so a redeploy is quick.

## 10. Web console

[ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md). Plain HTML, CSS and JavaScript, embedded in `xgi` at build time (`src/xgi/ui/`, `cmake/EmbedAssets.cmake`), and served at `http://<coordinator>:8100/ui/`.

- **Auth.** The page has no secrets and loads without a token. It asks for the cluster token, keeps it in `sessionStorage` (or `localStorage` if the operator ticks *Remember*), and sends `X-Cluster-Token` on every call. Static files are served with `Content-Security-Policy: default-src 'none'; script-src 'self'; …; frame-ancestors 'none'`, `X-Frame-Options: DENY` and `nosniff`. Node data is rendered as text only, never as HTML.
- **Dashboard.** It polls `GET /nodes` and `GET /groups` every 3 s. Node health combines liveness, draining, the group's status and the node's own process states (`Serving`, `Idle`, `Loading`, `Draining`, `Dead`, `<role> exited`). The node drawer shows identity, system facts, processes, llama.cpp binaries, models, and the languages table. It can also drain or undrain the node.
- **Code console.** The operator picks a node and one of its languages, and describes a task. `POST /console/generate` builds a system prompt with the node's OS, architecture and runtime version, sends it with up to 12 earlier turns to the router (pool `console.model`), and extracts the first fenced code block. The code appears in an editable box. **It is never run automatically.** *Run* calls `POST /console/exec`. The coordinator checks `console.allow_exec`, that the node is alive and has `exec_enabled`, and forwards the call to the agent with a timeout of `timeout_s + 30` s. *Write code myself* skips generation.
- **Logging.** Each generate and exec request is logged with the node, language, code size and caller address.

### 10.1 Configuration page

The console's **Configuration** tab (`/ui/#config`) edits `cluster.yaml` ([05 §5.1](05-protocols-and-data.md#51-where-xgi-finds-clusteryaml-and-changing-it-while-running)).

- **Form:** an editable pools table (name, model file, nodes per group, groups, context, slots) with a "needs N nodes, M alive" count, and fields for placement, the code console, llama.cpp, the coordinator and the router. Each section is labelled *applies now* or *after restart*. `tensor_split`, per-pool `extra_args` and `pinned` aren't in the form but are kept. The token is never sent to the browser. Saving from the form rewrites the file in a standard layout without comments.
- **YAML:** the file as text, saved byte-for-byte.
- **Validate** runs the startup validation without saving. **Save & apply** writes the file atomically (keeping `cluster.yaml.bak`), then calls `Coordinator::apply_layout`, which replaces the pools, spares, pinned and llama.cpp settings under the state mutex. Lifecycle tasks copy the llama.cpp settings under the lock, so a save never changes a group's start half-way. Running groups keep their members and model until a redeploy. The page offers **Redeploy now** after a layout change.
- The Cluster tab's Groups section has **Deploy**, **Redeploy** and **Undeploy** buttons (`POST /deploy`, `/deploy?force=true`, `/undeploy`).
