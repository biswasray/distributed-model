# 04 · Router (API Gateway)

> Component spec. A single instance, normally on the same machine as the coordinator. Port **8000**.

## 1. Responsibilities

1. Expose an **OpenAI-compatible API** to clients.
2. Map the request's `model` to a **pool**, and pick the **least-busy healthy group** in it.
3. **Queue** requests when every group in the pool is busy. Reject them when the queue is full.
4. **Proxy** requests to the group's head and **stream** responses back without buffering.
5. Optionally **authenticate clients** with API keys.

The router holds no durable state. Its picture of the cluster comes from polling the coordinator.

## 2. Cluster view

- The router polls `GET {coordinator_url}/groups` every **3 s** with the cluster token.
- It keeps a table `pool → [groups]` and only routes to groups with `status == healthy`.
- If the coordinator is unreachable, the router **keeps using the last known table**. Heads keep serving while the control plane is down. A group that fails during this time shows up as proxy errors, and the request is retried on another group (§4.3).

## 3. Request admission and queueing

Each group has a capacity of `slots` concurrent requests (the pool's `parallel`, defaulting to `router.slots_per_group`). The router counts **in-flight requests per group** itself.

```
request(model)
  ├─ unknown model / pool has no healthy groups ─► 503 {"error": "model unavailable"}
  ├─ a group has inflight < slots ─► dispatch to argmin(inflight / slots)
  └─ all busy
        ├─ queue length ≥ max_queue ─► 429 {"error": "queue full"}   + Retry-After
        └─ wait (FIFO per pool) up to queue_timeout_s
              ├─ slot freed ─► dispatch
              └─ timeout ─► 503 {"error": "timed out waiting for capacity"}
```

- Queues are **per pool**, so a backlog on the slow 72B pool doesn't block the fast pools.
- Ties go to the group that has been idle longest, which spreads wear and keeps caches warm.

## 4. Proxying

### 4.1 Mechanics

- The request body is forwarded **unchanged** to `<head_url><same path>`. llama-server ignores `model`.
- `stream: true` → response bytes (SSE `data:` lines) are relayed as they arrive. `stream: false` → the JSON is returned as is.
- The upstream timeout is `request_timeout_s` (default 1800 s, because 72B answers can take many minutes).
- When the client disconnects, the router **cancels the upstream request**, so llama-server frees the slot.

### 4.2 Response headers added

| Header | Value |
|---|---|
| `X-Qwen-Pool` | Pool name |
| `X-Qwen-Group` | `group_id` that served the request |
| `X-Qwen-Queue-Ms` | Time spent waiting in the queue |

### 4.3 Retry policy

- A **connection error before any response bytes** → mark the group suspect locally for 30 s and retry once on another healthy group.
- A failure **after streaming has started** → no retry, because the client has already received tokens. The stream ends with an SSE error event.

## 5. Client API

| Method | Path | Behaviour |
|---|---|---|
| GET | `/v1/models` | The pools with ≥1 healthy group, in OpenAI list format |
| POST | `/v1/chat/completions` | Routed by `model` |
| POST | `/v1/completions` | Routed by `model` |
| GET | `/health` | Router liveness, plus per-pool healthy and in-flight counts |

Schemas are in [05-protocols-and-data.md](05-protocols-and-data.md#4-router-api).

## 6. Client authentication

- If `router.api_keys` is non-empty, clients must send `Authorization: Bearer <key>`. Otherwise they get **401**.
- If it's empty, the API is open. That's acceptable only on a trusted LAN ([06-security.md](06-security.md)).
- Client API keys are **separate** from the cluster token. Clients never see the cluster token.

## 7. Configuration keys (`router:`)

| Key | Default | Description |
|---|---|---|
| `host`, `port` | `0.0.0.0`, `8000` | Listen address |
| `coordinator_url` | `http://127.0.0.1:8100` | Where to poll `/groups` |
| `max_queue` | `100` | Maximum queued requests **per pool** |
| `queue_timeout_s` | `600` | Maximum time a request waits for capacity |
| `request_timeout_s` | `1800` | Upstream timeout |
| `slots_per_group` | `1` | Default concurrency for each group |
| `api_keys` | `[]` | Accepted client keys |

## 8. Scaling notes

- One router easily handles this cluster, because the bottleneck is inference, not proxying.
- For HA, run two routers behind a DNS name or a reverse proxy. They share no state. Each counts its own in-flight requests, so under heavy load they may slightly over-commit slots. llama-server queues the extra requests internally.
