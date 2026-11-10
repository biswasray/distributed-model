# 03 · Coordinator

> Component spec. A single instance on the control machine. Port **8100**.

## 1. Responsibilities

1. Keep the **node registry** up to date from registrations and heartbeats.
2. **Plan placement**: turn `cluster.yaml` pools into concrete groups of nodes.
3. Drive each group's **lifecycle**: start workers, start the head, wait for health, stop.
4. **Detect failures and repair** groups automatically.
5. Expose cluster state to the router and to operators.

## 2. State model

All state lives **in memory** ([ADR-006](../adr/ADR-006-in-memory-coordinator-state.md)).

### 2.1 Node record

| Field | Source | Notes |
|---|---|---|
| `node_id`, `ip`, `agent_port`, `rpc_port`, `head_port` | register | Identity and addresses |
| `ram_total`, `ram_available`, `cpu_physical` | register / heartbeat | Used by placement |
| `models[]` | register | GGUF files on the node's disk, relative to its models dir |
| `processes` | heartbeat | `{rpc, head}` → `stopped` / `running` / `exited` |
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
| `error` | Last failure reason, for operators |
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

A node that comes back after being marked dead re-registers and joins `free`. It doesn't automatically return to its old group.

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
| POST | `/groups/{id}/restart` | operator | Stop and start one group with the same members, if they're all alive |
| POST | `/drain/{node_id}` | operator | Exclude the node from placement. If it's in a group, the group is repaired without it. |
| POST | `/undrain/{node_id}` | operator | Make the node eligible again |

Every endpoint except `/health` requires `X-Cluster-Token`. Schemas are in [05-protocols-and-data.md](05-protocols-and-data.md#2-coordinator-api).

## 8. Concurrency rules

- One `asyncio.Lock` protects placement, so two repairs can never claim the same free node.
- Each group has at most one lifecycle task. A new start or repair cancels and replaces the old one.
- Calls to agents use timeouts (10 s), so one unreachable agent can't block the monitor loop.

## 9. Restart behaviour

When the coordinator restarts, it starts with no state. Agents get a 404 on their next heartbeat and re-register within about 5 seconds. Any llama.cpp processes still running are **not adopted**. The operator runs `POST /deploy` again, which restarts them. Workers reload from their tensor cache, so a redeploy is quick.
