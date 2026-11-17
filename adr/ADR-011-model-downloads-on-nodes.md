# ADR-011: Model downloads onto nodes from Hugging Face and GitHub

- **Status:** Proposed
- **Date:** 2026-11-17

## Context

[ADR-005](ADR-005-model-files-relative-to-models-dir.md) says every head must already hold its pool's GGUF file in `--models-dir`, and leaves getting it there to the operator. With about 50 PCs, that means copying multi-GB files by hand (USB sticks, file shares), with no view of which node has what and no way to resume a copy that breaks. Model files come from two places in practice: Hugging Face model repos and GitHub release assets.

## Decision

1. **The node downloads, not XGI.** XI gets a download manager (`POST /downloads`, pause, resume, cancel, `GET /hub/files`). The file goes straight into that node's models folder. XGI only orchestrates: the console's **Models** tab and the new **`xgi download`** CLI call `/console/downloads*` on the coordinator, which forwards to the chosen agents (one or many at once).
2. **Two sources, fixed hosts.** A request names a source (`huggingface` or `github`), a repo `owner/name`, a ref (branch/commit, or release tag; `latest` resolves to the newest release) and file paths. XI builds the URL itself (`https://huggingface.co/<repo>/resolve/<ref>/<file>`, `https://github.com/<repo>/releases/download/<tag>/<file>`), so the API can't be used to fetch arbitrary URLs. Paths are validated, and a file is saved as `<models_dir>/<dest_dir>/<file name>`, never outside the folder.
3. **The system `curl`, not a TLS library.** Windows 10 1803+ ships `curl.exe`, as do Linux and macOS. XI runs it as a supervised child process (the same `ManagedProcess` as llama.cpp), so XGI and XI still need no OpenSSL. A node without `curl` reports `downloads.available: false`.
4. **Resumable by design.** A file downloads to `<target>.part` and is renamed when complete and the size matches the hub listing. Pause kills curl and keeps the `.part`; resume (or any later download of the same file, even after an agent restart) continues it with an HTTP Range request (`curl -C -`). Cancel deletes the `.part`. Transient failures retry up to 5 times with backoff. An existing file is never overwritten.
5. **Tokens stay on the node.** Gated or private repos use `HF_TOKEN` (or `HUGGING_FACE_HUB_TOKEN`) and `GITHUB_TOKEN` (or `GH_TOKEN`) from the agent's environment. They are passed to curl in a header file, not on its command line, and are never sent through XGI.
6. **The coordinator learns about new files** because the agent re-registers after each completed file, so placement can use the node as a head at once.

## Alternatives considered

| Option | Why not |
|---|---|
| XGI downloads once, then copies to nodes over the LAN | One more copy of a 20–50 GB file over the shared Wi-Fi, and XGI would need disk space and a file-transfer protocol. A LAN copy may still be worth it later for nodes without internet access. |
| Link OpenSSL (or WinHTTP / NSURLSession per OS) into XI | A large dependency to build and patch on three OSes, or three OS-specific HTTP clients. curl is already on every supported OS. |
| Accept any URL | Turns every agent into a fetch-anything proxy for token holders (internal URLs, huge files). The two hub URL shapes cover the need. |
| `huggingface-cli` / `hf_transfer` | Needs Python on every node. |
| Store the HF token in `cluster.yaml` and send it with each request | Spreads a personal access token across the LAN on every call. Node environment variables keep it where it is used. |

## Consequences

- ✅ Operators fetch a model onto one or many nodes from a browser or one command, watch progress, and pause, resume or cancel.
- ✅ Interrupted downloads (Wi-Fi drop, agent restart, pause) continue where they stopped.
- ❌ Nodes need outbound HTTPS to `huggingface.co` (and its CDN) or `github.com` / `api.github.com` (and `objects.githubusercontent.com`).
- ❌ 50 nodes downloading the same 18 GB file at once share the site's internet link. Download to the heads only (the **Missing it** picker), a few at a time.
- ❌ Integrity is checked by size only, not by the hub's SHA-256.
- 🔜 Per-download rate limits, SHA-256 verification, and copying a file from a node that already has it.
