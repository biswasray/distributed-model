# ADR-010: XI web page and runtime settings in xi.yaml

- **Status:** Proposed
- **Date:** 2026-11-16

## Context

Every XI setting (coordinator URL, node name, folders, ports, code execution) was a command-line flag baked into the Task Scheduler task on each of about 50 PCs. Changing one meant RDP to the PC, re-running `install-xi-task.ps1`, and restarting the task. Operators also had no way to see a single node's view: is it registered, why not, what do its llama.cpp processes print. The XGI console ([ADR-009](ADR-009-web-console-and-node-code-execution.md)) shows the cluster, but not the agent's own state or settings.

## Decision

We will give XI its own web page and a settings file:

1. XI serves a page at `/ui/` on the agent port (8200). Like the XGI console, it is plain HTML/CSS/JS embedded in `xi` (it reuses XGI's stylesheet), loads without a token, and sends `X-Cluster-Token` on every API call. It shows registration, system facts, llama.cpp processes and their output, languages, and a settings form.
2. Settings live in **`xi.yaml`** (`--config`, else `./xi.yaml`, else next to `xi`). Precedence: defaults < `xi.yaml` < flags. `--coordinator` is no longer required: without it the agent starts, serves its page, and waits. The install scripts now write `xi.yaml` and start `xi --config <root>\xi.yaml`, so page edits persist.
3. `PUT /agent/settings` validates, writes `xi.yaml` atomically, and applies everything at once **except** `host`, `port`, `log_dir`, `rpc_port` and `head_port`, which apply after a restart. A live change re-registers with the coordinator.
4. The **token is never written to `xi.yaml`**. The page can replace it for the running process only.
5. **Turning code execution on is local-only.** `allow_exec: true` is accepted only from a loopback address (a browser on that node), from `--allow-exec`, or by editing `xi.yaml`. It can be turned off from anywhere. This keeps ADR-009's rule that the token alone is not enough to run code on a node.

## Alternatives considered

| Option | Why not |
|---|---|
| Keep flags only; re-run the install script | Needs RDP to each PC for every change, and a restart that drops its llama.cpp processes. |
| Manage agent settings centrally from XGI | Puts per-node state in the in-memory coordinator ([ADR-006](ADR-006-in-memory-coordinator-state.md)), and an unreachable node is exactly when you need to fix its coordinator URL locally. |
| Let the page enable `allow_exec` remotely | Then a token holder could turn on code execution on every node, collapsing ADR-009's two opt-ins into one. |
| Store the token in `xi.yaml` | Plain-text secret in a file readable through the page. The environment variable stays the single place. |
| Apply every setting live | Rebinding the API socket or moving llama.cpp ports while groups run would break them silently. |

## Consequences

- ✅ Operators can fix a node's coordinator URL, name, folders or exec settings from a browser, without a restart.
- ✅ A fresh install can start with no settings and be configured from `http://127.0.0.1:8200/ui/`.
- ❌ Two sources of truth at startup (flags and `xi.yaml`). The page marks settings that a flag will override at the next start.
- ❌ Renaming a node or changing its coordinator re-registers it. The old record goes dead after `node_timeout_s`, and its group is repaired.
- 🔜 A "restart agent" action, and bulk editing of agent settings from the XGI console.
