---
name: write-adr
description: Record an architecture decision for the Qwen cluster as a new ADR in docs/adr, and update the indexes and affected docs. Use when a decision changes a design invariant, adds a component or dependency, changes a public contract, or the user says "record this decision" or "write an ADR".
---

# Write an ADR

## Steps

1. **Confirm that it needs an ADR.** It does if it changes an invariant in `CLAUDE.md`, adds or removes a component or external dependency, breaks a contract in `architecture/05-protocols-and-data.md`, or reverses an earlier ADR. Small wording or doc fixes don't need one.
2. **Number it.** List `adr/ADR-*.md` and take the next number (zero-padded to 3 digits). File name: `ADR-NNN-short-kebab-title.md`.
3. **Fill in the template** from `adr/README.md`:
   - **Status:** `Proposed` until the user accepts it, then `Accepted`.
   - **Date:** today's date (absolute, YYYY-MM-DD).
   - **Context:** forces and constraints, with numbers where possible (RAM, latency, sizes).
   - **Decision:** one clear statement in the active voice ("We will …").
   - **Alternatives considered:** a table of options with *why not*.
   - **Consequences:** ✅ positive, ❌ negative, 🔜 follow-ups.
   Keep it under about one page.
4. **If it supersedes an ADR**, change only the old ADR's status line to `Superseded by ADR-NNN`. Don't edit the rest of it.
5. **Update the indexes**: the table in `adr/README.md` and the ADR list in the docs `README.md`.
6. **Spread the change.** Update `CLAUDE.md` (invariants and key numbers), the affected `architecture/*.md` specs, and the operator guide, so that no doc contradicts the new decision. Search the docs for the old terms.
7. Show the user the ADR and the list of files changed, and ask whether to mark it `Accepted`.

## Style

- Plain, specific language. Mention Windows, CPU-only, and Wi-Fi when they're the reason.
- Link related ADRs and specs with relative links.
- Don't put code in an ADR, beyond short config or command snippets that illustrate the decision.
