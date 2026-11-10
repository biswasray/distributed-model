# ADR-004: Shared cluster token for control-plane auth

- **Status:** Accepted
- **Date:** 2026-11-10

## Context

Node agents can start processes on 50 machines, so the control plane must be authenticated. The operators run a small LAN without a PKI or a directory service.

## Decision

Every control-plane call (agent ⇄ coordinator, router → coordinator) carries **`X-Cluster-Token: <secret>`**. The secret comes from the `QWEN_CLUSTER_TOKEN` environment variable, falling back to `cluster_token` in `cluster.yaml`. Client access to the router uses **separate** API keys.

## Alternatives considered

| Option | Why not |
|---|---|
| No auth | Anyone on the LAN could run processes on every node |
| mTLS per node | Needs certificate issuing and rotation. Too heavy for the operators. |
| Windows domain / Kerberos | Machines may not be domain-joined |

## Consequences

- ✅ Trivial to deploy: one environment variable per machine.
- ❌ A single shared secret: one leak exposes the whole cluster, and rotation needs a short outage ([06-security.md §5](../architecture/06-security.md#5-token-rotation)).
- ❌ The token is sent in plaintext over HTTP → rely on WPA2/WPA3 and the LAN boundary.
- ⚠️ It does **not** protect llama.cpp's own ports (8080, 50052). Those need firewall rules.
