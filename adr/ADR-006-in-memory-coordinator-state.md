# ADR-006: Coordinator state kept in memory only

- **Status:** Accepted
- **Date:** 2026-11-10

## Context

The coordinator tracks nodes and groups. Persisting that state (SQLite, etcd) adds work around schemas, migrations and stale records. Agents already re-send their full state every few seconds.

## Decision

Keep all coordinator state **in memory**. After a restart:

- agents get `404` on their heartbeat and **re-register** within about 5 seconds;
- groups are **not** recovered automatically. The operator runs `POST /deploy`, and workers reload quickly from their tensor caches.

`cluster.yaml` is the only durable source of truth for *desired* state.

## Alternatives considered

| Option | Why not |
|---|---|
| SQLite snapshot | Risk of acting on stale group membership after a restart. More code. |
| Adopting running processes from heartbeats | More complex rebuild logic. Can be added later without breaking the API. |

## Consequences

- ✅ Simple, and no stale state.
- ❌ A coordinator restart interrupts placement and repair until someone redeploys. Heads keep serving, and the router uses its last known table.
- 🔜 An optional `--auto-deploy` flag that redeploys once enough nodes have registered.
