# ADR-003: Qwen3-30B-A3B (MoE) as the default pool

- **Status:** Accepted
- **Date:** 2026-11-10

## Context

Users asked for Qwen 72B. On this hardware, 72B Q4 runs at about 0.3–0.6 tokens/s, so a 300-word answer takes 10–20 minutes (ADR-002). Mixture-of-experts models read only their *active* parameters for each token.

| Model | Size Q4 | Active params | Nodes/group | Estimated tok/s |
|---|---|---|---|---|
| Qwen2.5-72B | ~47 GB | 72B | 8 | 0.3–0.6 |
| **Qwen3-30B-A3B** | ~18.6 GB | ~3B | **3** | **5–10** |
| Qwen2.5-7B | ~4.7 GB | 7B | 1 | 5–8 |

## Decision

Make **`qwen3-30b-a3b`** the default and largest pool (about 10–16 groups). Offer `qwen-72b` as a small optional pool for tasks that really need it, and `qwen-7b` for high-volume simple tasks.

## Consequences

- ✅ Quality is close to 72B for most tasks, at about 15× the speed per request.
- ✅ Small groups (3 nodes) mean fewer hops and fewer points of failure.
- ❌ Answers are not identical to 72B. Some tasks will still need the 72B pool.
- 🔜 Evaluate larger MoE models (e.g. Qwen3-235B-A22B, about 22 nodes per group) as a higher-quality pool.
