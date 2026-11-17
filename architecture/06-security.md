# 06 · Security

> Threat model and controls. The cluster is designed for a **trusted private LAN** only.

## 1. Assets

| Asset | Why it matters |
|---|---|
| Node machines | The agent can start processes, so taking over the control plane means running workloads on 50 PCs. With `--allow-exec`, it means running **any code** on those nodes. |
| Prompts and responses | May contain sensitive user data, and travel over Wi-Fi |
| Cluster token | Gives full control-plane access |
| Model files | Large, but public. Low sensitivity. |

## 2. Trust boundaries

```
 Untrusted ─┤ Internet │─ never exposed
            ─────────────
 Semi-trusted ─┤ LAN clients │──(API key)──► Router :8000
            ─────────────────────────────────────────────
 Trusted control plane ─ Coordinator :8100 ⇄ Agents :8200   (cluster token)
 Trusted data plane    ─ Head :8080 ⇄ rpc-server :50052     (NO auth → firewall)
```

## 3. Threats and controls

| # | Threat | Control | Residual risk |
|---|---|---|---|
| T1 | Anyone on the LAN connects to `rpc-server :50052` and runs work or reads memory | Firewall: allow 50052 **only from cluster IPs** on the Private profile | **High if the firewall is misconfigured.** llama.cpp RPC has no auth or encryption. |
| T2 | A rogue caller tells an agent to start processes | `X-Cluster-Token` on every agent call. Without `--allow-exec`, agents only launch the configured llama.cpp binaries. | Token theft = control. Rotate it if leaked. |
| T3 | Path traversal through `model_file` | Resolved inside `--models-dir`, otherwise rejected ([ADR-005](../adr/ADR-005-model-files-relative-to-models-dir.md)) | Low |
| T4 | `extra_args` abused (e.g. `--log-file` to an arbitrary path) | Only callers that hold the token. Set from `cluster.yaml` only. | Accepted |
| T5 | A rogue node registers and gets assigned prompts | Registration needs the token | A machine with the token is trusted |
| T6 | Eavesdropping on Wi-Fi | WPA2/WPA3 on the network | Prompts are plaintext inside the LAN |
| T7 | Unauthorized clients use the API | `router.api_keys` | Keys are shared secrets |
| T8 | Exposure to the internet | Never port-forward. Put a reverse proxy with TLS and auth in front if remote access is needed. | — |
| T9 | Denial of service by flooding requests | Per-pool `max_queue` → 429 | Not defended against a determined attacker |
| T10 | Console code execution abused, or a generated program does damage ([ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md)) | Off by default. Needs both `console.allow_exec: true` and `xi --allow-exec`. Token on every call. Generated code is shown for review and never auto-run. Timeout, 2 jobs per node, 256 KB code limit, process tree killed at timeout. Every run is logged. | **High on exec-enabled nodes:** the token = code execution as the agent's account (SYSTEM for the scheduled task). No sandbox, no per-user identity. |
| T11 | A malicious page or node data attacks the operator's browser | Console served with a strict CSP, `X-Frame-Options: DENY`, `nosniff`. All node data rendered as text. The API needs a custom header, so other sites can't call it cross-origin. | The token in `localStorage` (only if *Remember* is ticked) can be read by anyone using that browser profile |
| T12 | The cluster config is changed through the console (for example `console.allow_exec` turned on, or pools pointed at another model file) | `PUT /console/config` needs the token. The same validation as at startup. Every save is logged with the caller's address, and the old file is kept as `cluster.yaml.bak`. The form never receives the token; the YAML view shows the file as it is, so keep the token in `QWEN_CLUSTER_TOKEN` rather than in `cluster.yaml`. | A token holder can already control the cluster. Turning on `allow_exec` still needs `xi --allow-exec` on each node. |
| T13 | A token holder changes a node's settings on its agent page (another coordinator URL, other folders, or code execution) ([ADR-010](../adr/ADR-010-agent-web-page-and-runtime-settings.md)) | `PUT /agent/settings` needs the token and is validated and logged with the caller's address; `xi.yaml.bak` keeps the previous file. **Code execution can only be turned on from the node itself** (loopback), with `--allow-exec`, or in `xi.yaml`. The token is never stored in `xi.yaml`. | A token holder can point a node at a rogue coordinator, which already holds the same power over that node. |
| T14 | Model downloads abused: fetching arbitrary URLs through a node, writing outside the models folder, overwriting a model in use, filling the disk, or leaking a Hugging Face token ([ADR-011](../adr/ADR-011-model-downloads-on-nodes.md)) | The token on every call. The agent builds the URL itself from a repo, ref and file on `huggingface.co` or `github.com` only, with validated characters (no `..`, `\`, `:`). Files go to `<models_dir>/<dest_dir>/<name>` and never replace an existing file. A disk-space check before starting, 3 downloads per node, 64 files per job. Hub tokens live only in the agent's environment and reach curl through a temporary header file. Every download is logged with the caller's address. | A token holder can fill a node's disk with many different files, or download a file someone has tampered with on the hub (the size is checked, not the SHA-256). |

## 4. Required hardening checklist

- [ ] The Wi-Fi network profile is **Private** on every node.
- [ ] Firewall rules for 8080, 8200 and 50052 use `-RemoteAddress <cluster subnet>`.
- [ ] `QWEN_CLUSTER_TOKEN` is set as a machine environment variable (a long random string) and is **not** committed in `cluster.yaml`.
- [ ] `router.api_keys` is set if anyone other than operators can reach port 8000.
- [ ] No router port forwarding or UPnP for these ports.
- [ ] The same llama.cpp release is on all nodes, from the official GitHub releases only.
- [ ] Agent port 8200 (API and web page) is reachable only from the cluster subnet. Code execution is turned on per node only by someone at that node.
- [ ] Nodes that download models can reach `huggingface.co` / `github.com` over HTTPS. Set `HF_TOKEN` (a read-only token) only on nodes that need gated models, as a machine environment variable.
- [ ] `console.allow_exec` stays `false` unless you need it. If you turn it on, start `xi --allow-exec` only on the nodes that need it, and preferably run those agents as a dedicated non-admin user instead of SYSTEM.

## 5. Token rotation

1. Generate a new token.
2. Update `QWEN_CLUSTER_TOKEN` on every node and on the control machine.
3. Restart the agents (the scheduled task), then the coordinator and the router.

While this is in progress, nodes with the old token fail heartbeats and are marked dead. Plan a short maintenance window.
