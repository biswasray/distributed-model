# ADR-009: Web console on XGI and opt-in code execution on nodes

- **Status:** Proposed
- **Date:** 2026-11-16

## Context

Operators watch about 50 Windows PCs through `curl.exe` calls to `GET /nodes` and `GET /groups`. They want one page that shows every node's health, its hardware and OS, and which programming languages it can run. They also want to describe a task in a chat box, have the cluster's own Qwen model write a program for it, and run that program on a chosen node.

Running arbitrary code breaks a control in [06-security.md](../architecture/06-security.md) T2: "agents only launch the configured llama.cpp binaries". Anyone with the cluster token could then run any program on any node that allows it. Nodes that serve a group would also share CPU and RAM (about 6.5 GB usable) between inference and these programs.

## Decision

We will add a **web console** to XGI and an **opt-in code runner** to XI:

1. The coordinator (port 8100) serves a plain HTML/CSS/JS page at `/ui/`. The files are embedded in `xgi` at build time, so it stays a single binary with no new dependency. The page itself needs no token. It asks for the cluster token and sends `X-Cluster-Token` on every API call ([ADR-004](ADR-004-shared-cluster-token.md)). It is served with a strict Content-Security-Policy.
2. XI detects the language runtimes on its `PATH` at startup (for example Python, Node.js, PowerShell, cmd, Bash, Java, Go, C/C++ and Rust). It reports them in its registration as `runtimes`, along with `arch`, `cpu_model`, `disk_free` and `binaries`.
3. `POST /console/generate` sends the prompt to the router's `/v1/chat/completions`, using pool `console.model`. Code is generated **on the cluster** and is never sent outside the LAN.
4. `POST /console/exec` forwards reviewed code to the node agent's new `POST /exec`. That endpoint runs it with a detected runtime in `--exec-dir`, with a timeout, captured output, and at most 2 concurrent jobs. On Windows the job runs in a kill-on-close job object, and on POSIX in its own process group.
5. The console has a **Configuration** page that validates and saves `cluster.yaml` and applies pools, spares, pinned, llama.cpp and console settings without a restart. Ports, the token and router settings apply on the next start. Without a `cluster.yaml` (none given, none in the current folder or next to `xgi`), XGI starts with built-in defaults and no pools, so a fresh install can be configured from the browser.
6. Execution is **off by default, and needs two opt-ins**: `console.allow_exec: true` in `cluster.yaml` **and** `xi --allow-exec` on each node. The UI never runs generated code automatically. The operator reviews and edits it, then clicks Run.

## Alternatives considered

| Option | Why not |
|---|---|
| Separate web server or SPA framework (React, Node.js) | A new runtime to install on the control machine, and a build toolchain. Vanilla JS, embedded in `xgi`, keeps the single-binary deploy. |
| Execution on by default | Turns every token leak into remote code execution on 50 PCs. Opt-in on both sides limits the blast radius to nodes the operator chose. |
| Auto-run generated code | LLM output can be wrong or destructive. A human review step is cheap. |
| Sandbox (containers, WSL, AppContainer) | Not available on every Windows node. Not portable to Linux and macOS ([ADR-008](ADR-008-cross-platform-build.md)). Listed as follow-up. |
| Restart XGI to apply every config change | A restart drops the coordinator's in-memory state ([ADR-006](ADR-006-in-memory-coordinator-state.md)) and needs a redeploy. Applying the layout sections live avoids that. Ports and the token stay restart-only because they are bound into sockets and agents. |
| Agent pulls jobs from the coordinator | Adds a job queue and state to the in-memory coordinator ([ADR-006](ADR-006-in-memory-coordinator-state.md)). A direct, synchronous call matches the existing start/stop calls. |

## Consequences

- ✅ One page shows cluster health, per-node system facts and languages, and lets operators run tasks on nodes without RDP.
- ✅ Additive contract changes only: new endpoints and new optional registration fields. Old agents still work and show no languages.
- ❌ On nodes with `--allow-exec`, the cluster token now means **code execution as the agent's user**. Programs run unsandboxed, with the agent's rights and network access.
- ❌ A program on a serving node competes with llama.cpp for CPU and RAM. The UI warns about this.
- ❌ Output is capped at the last 500 lines, and runs at 300 s (`--exec-max-timeout-s`). The XI startup is about 1–5 s slower while runtimes are probed.
- 🔜 Per-user authentication and an audit trail beyond the log lines. Optional sandboxing (a low-privilege user, a Windows job with memory limits). Streaming output for long jobs.
