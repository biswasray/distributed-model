# ADR-005: Model files pre-staged on heads, referenced by relative path

- **Status:** Accepted
- **Date:** 2026-11-10

## Context

GGUF files are 5–50 GB. Sending them over shared Wi-Fi is slow and congests the network for everyone. The `llama-server` on a head must read the full file from its local disk. Letting the coordinator pass absolute paths to agents would allow path traversal.

## Decision

1. Operators **pre-stage** GGUF files on the intended head nodes (USB drive or a wired copy) in each agent's `--models-dir`.
2. Agents report the `*.gguf` files they have at registration. The coordinator only places a head on a node that **already has** the pool's `model_file`.
3. `model_file` in `cluster.yaml` is a path **relative to the models dir**. Agents resolve it and reject anything outside that folder.
4. Workers never need the file. They receive their slice over RPC once and cache it (`rpc-server -c`).

## Consequences

- ✅ No large automatic transfers over Wi-Fi. Predictable network load.
- ✅ Agents are protected against path traversal.
- ✅ Paths can differ between nodes. Only the relative name has to match.
- ❌ Manual work to stage the files. The number of nodes with the file limits how many groups a pool can have.
- ❌ The first group start still sends each worker's slice over Wi-Fi (up to about 1 hour for 72B).
- 🔜 An optional "pre-shard" tool that writes worker caches offline.
