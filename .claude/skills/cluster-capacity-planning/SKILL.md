---
name: cluster-capacity-planning
description: Size a Qwen model or pool for the Windows/Wi-Fi Qwen cluster: nodes per group, number of groups, spare count, and estimated tokens/s. Use when the user asks "how many machines for model X", "how fast will X be", "can we run X", or when docs need performance numbers.
---

# Cluster capacity planning

Work out how a model fits on the cluster, and estimate its speed. **Always label the results as estimates.**

## Inputs to collect (ask if unknown)

| Input | Default for this cluster |
|---|---|
| Model and quantization | Q4_K_M |
| GGUF file size `S` (GB) | From the Hugging Face file listing, or the table below |
| Free RAM per node | 8 GB |
| Usable RAM per node for weights `R` | 8 GB − ~1.5 GB (OS, KV cache, buffers) ≈ **6.5 GB** |
| Active parameters per token (MoE) | The full parameter count for dense models |
| Node RAM bandwidth `B` | **25 GB/s** (range 20–40) |
| Wi-Fi hop latency `L` | **15 ms** (range 5–30) |
| Total nodes `N`, spares `P` | 50, 2 |

Reference sizes (Q4_K_M): Qwen2.5-7B ≈ 4.7 GB · Qwen2.5-14B ≈ 9 GB · Qwen3-30B-A3B ≈ 18.6 GB (3B active) · Qwen2.5-72B ≈ 47 GB · Qwen3-235B-A22B ≈ 140 GB (22B active).

## Formulas

1. **Nodes per group**: `k = ceil(S / R)`. Round up by one more if the context is over 8k or `parallel > 1`, because the KV cache grows on every node.
2. **Groups**: `G = floor((N − P) / k)`.
3. **Bytes read per token**:
   - dense: `S`
   - MoE: `S × (active_params / total_params)` (approximate)
4. **Time per token**: `t = bytes_per_token / B + (k − 1) × L`. (`k − 1` hops, plus the return to the head, is already roughly covered by `L`.)
5. **Speed**: `tok/s ≈ 1 / t`. Report it as a range using the low and high ends of `B` and `L`.
6. **Time for an answer**: a 300-word answer ≈ 400 tokens → `400 × t`.
7. **Cluster throughput**: `G × tok/s` (when every group is busy).

## Worked example: Qwen2.5-72B Q4

- `k = ceil(47 / 6.5) = 8` · `G = floor(48 / 8) = 6`
- `t = 47/25 + 7 × 0.015 ≈ 1.88 + 0.11 ≈ 2.0 s` → **≈ 0.5 tok/s** (range ~0.3–0.6)
- A 300-word answer takes **≈ 13 min**. Throughput ≈ 3 tok/s across the cluster.

## Output format

Present a table with these columns: model · quant · size · k · groups · estimated tok/s (range) · 300-word time · concurrency. Then give a one-line recommendation.

## Rules

- Never suggest adding nodes to a group to increase speed. It only adds hops (ADR-002).
- If `k > 12`, warn that Wi-Fi jitter and failures add up, and suggest a MoE or smaller model.
- If the numbers go into the docs, update all of `CLAUDE.md` (key numbers), `distributed-qwen-cluster.md` §3 and §11, and ADR-003 if the default pool changes. Changing an accepted ADR's decision needs a new ADR.
