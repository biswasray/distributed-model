---
name: add-model-pool
description: Add, resize, or remove a model pool (e.g. a new Qwen model) in the Qwen cluster design, keeping cluster.yaml examples and every affected doc consistent. Use when the user wants to serve a new model, change groups/nodes_per_group, or retire a pool.
---

# Add or change a model pool

A pool = one model name that clients choose with `model`, served by `groups` groups of `nodes_per_group` nodes.

## Steps

1. **Size it.** Run the `cluster-capacity-planning` skill to get `nodes_per_group`, a realistic `groups`, and estimated speed.
2. **Check the node budget.** `Σ(nodes_per_group × groups)` over all pools, plus `spares`, must be ≤ the number of nodes (50). If it isn't, cut groups from the least important pool and tell the user the trade-off.
3. **Pick the identifiers.**
   - `name`: lowercase, hyphenated, and stable (clients depend on it), e.g. `qwen3-30b-a3b`.
   - `model_file`: a path **relative to the models dir**. For split GGUFs, use the `-00001-of-0000N` part (ADR-005).
4. **Write the pool entry** using the exact schema in `architecture/05-protocols-and-data.md §5`:
   ```yaml
   - name: <name>
     model_file: <file>.gguf
     nodes_per_group: <k>
     groups: <G>
     context: 4096
   ```
   Pools are filled **in order**, so put the most important one first.
5. **Model staging note.** Tell the operator how many head nodes need the GGUF file (at least `groups` + 1 spare) and the file size. Recommend copying by USB or a wired connection.
6. **Update the docs** (search for the old pool names to find every reference):
   - `distributed-qwen-cluster.md`: §3.2 sizing table, §4 options, §7.4 download table, §8 config example, §11 performance table
   - `architecture/05-protocols-and-data.md`: config example (only if the schema changed)
   - `CLAUDE.md`: the key numbers table
   - `README.md` (docs index), if the pool appears there
7. **ADR check.** Write a new ADR (skill `write-adr`) if the **default** pool changes (this supersedes ADR-003), if the model needs a different engine or flags, or if it breaks an invariant in `CLAUDE.md`.

## Removing a pool

Same steps in reverse. Note that clients using that `model` name will get **503** errors, and suggest keeping an alias pool for a while if needed.

## Don't

- Use absolute paths in `model_file`.
- Write implementation code. Only docs and config examples, unless the user explicitly asks.
