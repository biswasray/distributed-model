# 06 · Security

> Threat model and controls. The cluster is designed for a **trusted private LAN** only.

## 1. Assets

| Asset | Why it matters |
|---|---|
| Node machines | The agent can start processes, so taking over the control plane means running workloads on 50 PCs |
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
| T2 | A rogue caller tells an agent to start processes | `X-Cluster-Token` on every agent call. Agents only launch the configured llama.cpp binaries. | Token theft = control. Rotate it if leaked. |
| T3 | Path traversal through `model_file` | Resolved inside `--models-dir`, otherwise rejected ([ADR-005](../adr/ADR-005-model-files-relative-to-models-dir.md)) | Low |
| T4 | `extra_args` abused (e.g. `--log-file` to an arbitrary path) | Only callers that hold the token. Set from `cluster.yaml` only. | Accepted |
| T5 | A rogue node registers and gets assigned prompts | Registration needs the token | A machine with the token is trusted |
| T6 | Eavesdropping on Wi-Fi | WPA2/WPA3 on the network | Prompts are plaintext inside the LAN |
| T7 | Unauthorized clients use the API | `router.api_keys` | Keys are shared secrets |
| T8 | Exposure to the internet | Never port-forward. Put a reverse proxy with TLS and auth in front if remote access is needed. | — |
| T9 | Denial of service by flooding requests | Per-pool `max_queue` → 429 | Not defended against a determined attacker |

## 4. Required hardening checklist

- [ ] The Wi-Fi network profile is **Private** on every node.
- [ ] Firewall rules for 8080, 8200 and 50052 use `-RemoteAddress <cluster subnet>`.
- [ ] `QWEN_CLUSTER_TOKEN` is set as a machine environment variable (a long random string) and is **not** committed in `cluster.yaml`.
- [ ] `router.api_keys` is set if anyone other than operators can reach port 8000.
- [ ] No router port forwarding or UPnP for these ports.
- [ ] The same llama.cpp release is on all nodes, from the official GitHub releases only.

## 5. Token rotation

1. Generate a new token.
2. Update `QWEN_CLUSTER_TOKEN` on every node and on the control machine.
3. Restart the agents (the scheduled task), then the coordinator and the router.

While this is in progress, nodes with the old token fail heartbeats and are marked dead. Plan a short maintenance window.
