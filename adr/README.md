# Architecture Decision Records

Each ADR records one significant decision: its context, what was decided, and the consequences. ADRs are **immutable once accepted**. To change a decision, write a new ADR that *supersedes* the old one, and update the old one's status line.

| ADR | Title | Status |
|---|---|---|
| [ADR-001](ADR-001-llama-cpp-as-inference-engine.md) | llama.cpp (RPC backend) as the inference engine | Accepted |
| [ADR-002](ADR-002-pipeline-not-tensor-parallelism.md) | Pipeline parallelism plus replicas, not tensor parallelism | Accepted |
| [ADR-003](ADR-003-moe-model-as-default-pool.md) | Qwen3-30B-A3B (MoE) as the default pool | Accepted |
| [ADR-004](ADR-004-shared-cluster-token.md) | Shared cluster token for control-plane auth | Accepted |
| [ADR-005](ADR-005-model-files-relative-to-models-dir.md) | Model files pre-staged on heads, referenced by relative path | Accepted |
| [ADR-006](ADR-006-in-memory-coordinator-state.md) | Coordinator state kept in memory only | Accepted |
| [ADR-007](ADR-007-cpp-implementation-xgi-xi.md) | Implement the control plane in C++ as XGI and XI | Accepted |

## Template

```markdown
# ADR-NNN: <Title>

- **Status:** Proposed | Accepted | Superseded by ADR-XXX
- **Date:** YYYY-MM-DD

## Context
What forces are at play? What problem are we solving?

## Decision
What we will do.

## Alternatives considered
| Option | Why not |

## Consequences
Positive, negative, and follow-ups.
```
