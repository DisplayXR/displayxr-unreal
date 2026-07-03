# Architecture Decision Records

Short decision logs for load-bearing choices in the DisplayXR Unreal plugin. Each ADR captures the context, the decision, the alternatives considered, and the consequences — so an agent picking up new work doesn't re-propose the paths we already ruled out.

## Index

- [ADR-001 — Direct OpenXR runtime loading (no UE OpenXR plugin)](./ADR-001-direct-runtime-loading.md)
- [ADR-002 — Zero-copy atlas handoff via single D3D12 device](./ADR-002-zero-copy-atlas-handoff.md)
- [ADR-003 — UE-native off-axis projection instead of Kooima's `projection_matrix[16]`](./ADR-003-ue-native-off-axis-projection.md)

## When to write a new ADR

- A decision is non-obvious and another agent would plausibly re-propose the alternative.
- The decision ties together code + docs + downstream behavior in a way that's hard to reconstruct from git history.
- You want to make "why not X?" easy to answer without re-running the investigation.

Keep ADRs short (under a page). Status, context, decision, alternatives considered, consequences. If it grows past a page, break it into a design doc and leave the ADR as the index.
