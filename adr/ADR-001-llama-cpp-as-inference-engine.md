# ADR-001: llama.cpp (RPC backend) as the inference engine

- **Status:** Accepted
- **Date:** 2026-11-10

## Context

The fleet is about 50 **Windows** PCs with about 8 GB free RAM each, GPUs too small to use, and **Wi-Fi** networking. We need to run Qwen models up to 72B, some of them split across machines. Operators are not ML infrastructure specialists.

## Decision

Use **llama.cpp**. `llama-server` runs on each group's head, and `rpc-server` runs on the workers. Models are **GGUF, quantized to Q4_K_M** by default.

## Alternatives considered

| Option | Why not |
|---|---|
| vLLM + Ray | Linux only, NVIDIA GPUs required |
| exo | Weak Windows support at the time of writing |
| distributed-llama | Tensor parallelism (see ADR-002). Linux or WSL. Node count must be a power of 2. |
| Petals | Built for public swarms over the internet. Heavy Python and CUDA stack. |
| Ollama | No multi-node model splitting. Still a good option for single-node pools in the future. |
| Custom engine | Far too much effort |

## Consequences

- ✅ Native Windows binaries, CPU-optimized (AVX2), with first-class GGUF quantization.
- ✅ `llama-server` already provides an OpenAI-compatible API and `/health`, so the router only proxies.
- ✅ `rpc-server -c` caches tensors on local disk, which avoids re-sending weights over Wi-Fi.
- ❌ The RPC protocol has **no auth or encryption** → firewall controls are needed ([06-security.md](../architecture/06-security.md)).
- ❌ RPC compatibility across versions isn't guaranteed → **all nodes must run the same llama.cpp build**.
- ❌ The head needs the **entire** GGUF file on its own disk.
