# ADR-012: Copying model files between nodes over the LAN

- **Status:** Proposed
- **Date:** 2026-11-20

## Context

[ADR-011](ADR-011-model-downloads-on-nodes.md) lets every node download GGUF files from Hugging Face or GitHub, and only from there. The default pool (Qwen3-30B-A3B, ~18.6 GB) has one head per group, so 16 groups means 16 heads, each fetching the same ~18.6 GB. That is about 300 GB over the site's internet link, and 470 GB for a 72B pool (~47 GB per head, 10 heads). Some lab PCs have no internet access at all. Meanwhile the file is often already on the LAN: on the XGI machine (`@xgi`, which is usually wired), or on a head that downloaded it first. ADR-011 listed "copying a file from a node that already has it" as a follow-up.

## Decision

We will add a third download source, **`peer`**: a node copies a GGUF file from another node, or from the XGI machine, over the LAN with the same download manager.

1. **Every agent, and XGI, serves its models folder read-only**: `GET /models` lists the `.gguf` files with their sizes, and `GET /models/file/<path>` sends one, with HTTP Range support. Both need `X-Cluster-Token`. Only complete `.gguf` files that resolve inside the models folder are served (no `.part` files, no symlinks out of the folder, nothing else). A server sends at most **2 files at once** (XGI: 4). A further request gets `429` with `Retry-After`. XGI serves `console.models_dir` on the coordinator port 8100. No new port or firewall rule is needed.
2. **The receiver pulls, with curl**, exactly as for the hubs: `.part` file, `-C -` resume, pause, cancel, a size check, never overwriting a file. `429`/`503` from a busy peer is not a failure: the job waits and tries again. The cluster token goes to curl in a header file, and curl does not follow redirects for peers, so the token is never sent anywhere else.
3. **Peers are named, not given as arbitrary URLs.** Through the console, the operator picks a node id, `@xgi`, or `auto`. The coordinator turns a node id into its registered `http://<ip>:<agent_port>`. `@xgi` is sent as is, and the receiving agent uses its own `coordinator` URL. An agent also accepts a `peer_url` directly, but only `http://` with an IPv4 address in a private, loopback or link-local range (10/8, 172.16/12, 192.168/16, 127/8, 169.254/16), and it fetches only the two fixed paths above. So the token is never sent to the internet.
4. **`auto` spreads the copies.** The coordinator picks, for each receiver, a source that has every requested file: idle nodes first, then the XGI machine, then nodes running llama.cpp. It assigns them round robin, so 16 heads copy from several sources instead of one. Agents report `model_sizes` in `/register`, which tells the coordinator which files each node can serve.
5. **The relative path is kept.** A file copied from a peer lands at `<models_dir>/<dest_dir>/<path on the peer>`, so the same pool `model_file` works on the copy ([ADR-005](ADR-005-model-files-relative-to-models-dir.md)).

## Alternatives considered

| Option | Why not |
|---|---|
| SMB file share on the XGI machine (`robocopy`) | Windows-only, needs share permissions and accounts on 50 PCs, has no progress in the console, and isn't resumable through XGI. |
| Peer-to-peer swarm (BitTorrent-style chunks) | Much more code, and a second protocol to secure. Round-robin `auto` plus "copy to a few, then from those" gets most of the gain. |
| XGI relays every copy | Every byte would cross the XGI machine's link twice, and it becomes the bottleneck. |
| Accept any `peer_url` | Lets a token holder make a node send the cluster token to any host, including the internet. Fixed paths plus private IPv4 addresses keep it inside the LAN. |
| A new port for file serving | One more firewall rule on every node. Port 8200 (agent) and 8100 (XGI) already carry token-checked HTTP. |

## Consequences

- ✅ A model crosses the internet once, onto XGI or one node. The other heads copy it over the LAN. Nodes without internet access can get models.
- ✅ Same UI, CLI, progress, pause/resume and safety rules as hub downloads, with no new dependency or port.
- ❌ On Wi-Fi a copy uses airtime twice (sender → access point → receiver) and competes with inference traffic. Copy from a wired machine (usually XGI) where possible, a few nodes at a time.
- ❌ A node serving a file uses disk and network while it may be running llama.cpp. `auto` prefers idle sources, and the 2-upload limit caps the load.
- ❌ Integrity is still checked by size only. A corrupt file on the source spreads to its copies.
- 🔜 SHA-256 verification (for hub and peer copies), per-copy rate limits, and fan-out ("tree") copies that use finished receivers as sources.
