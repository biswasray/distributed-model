# ADR-002: Pipeline parallelism plus replicas, not tensor parallelism

- **Status:** Accepted
- **Date:** 2026-11-10

## Context

We want the cluster to be "fast". There are two ways to split one model across nodes:

- **Pipeline parallelism:** each node holds a slice of layers. Each token crosses `k−1` network hops.
- **Tensor parallelism:** each layer is split across nodes. It needs about 2 synchronizations **per layer** per token (about 160 for an 80-layer model).

On CPU nodes, token speed is limited by memory bandwidth (~20–40 GB/s). Wi-Fi latency is 5–30 ms per round trip.

Estimated time per token for Qwen 72B Q4 (~47 GB):

| Strategy | Compute | Network | Total |
|---|---|---|---|
| Pipeline, 8 nodes | 47 GB ÷ 25 GB/s ≈ 1.9 s | 8 × ~15 ms ≈ 0.12 s | **≈ 2 s** |
| Tensor parallel, 8 nodes | ≈ 0.24 s (bandwidth adds up) | 160 × ~15 ms ≈ 2.4 s | **≈ 2.6 s, unpredictable** |

## Decision

- Use **pipeline parallelism** only to make a model **fit** in memory.
- Get speed through **throughput**: run many independent **replicas (groups)** behind a router.
- Keep groups as **small** as the model allows. Never add nodes to a group to make it "faster".

## Alternatives considered

| Option | Why not |
|---|---|
| Tensor parallelism over Wi-Fi | Network latency outweighs the bandwidth gain, and it fails badly when Wi-Fi jitters |
| One giant group of 50 nodes | No speed gain, 50 hops, and one failure stops everything |

## Consequences

- ✅ Scales throughput almost linearly with the number of groups.
- ✅ A failure affects only one group.
- ❌ A single request is never faster than one node's RAM bandwidth allows. This has to be explained clearly to users ([setup guide §11](../distributed-qwen-cluster.md#11-performance-expectations)).
- 🔜 Follow-ups: placement that keeps each group on one access point. Speculative decoding for the 72B pool.
