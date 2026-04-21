# ADR-003 — UE-native off-axis projection instead of Kooima's `projection_matrix[16]`

**Status:** Accepted
**Date:** 2026-04

## Context

The Kooima projection library (shared with [displayxr-unity](https://github.com/DisplayXR/displayxr-unity)) computes everything we need for asymmetric off-axis stereo: per-eye position, per-eye projection matrix, per-eye view matrix. The obvious integration is: take Kooima's 4×4 `projection_matrix[16]` and hand it to UE's stereo rendering pipeline.

We tried that. It fought UE at every step.

Kooima produces an OpenGL-convention projection matrix: column-major, right-handed, Z forward, depth `[-1, 1]`. UE uses row-major, left-handed, reverse-Z (`[1, 0]`). Every conversion we applied caused a subtle regression somewhere: inverted depth, flipped handedness in a post-process, broken planar reflections, etc.

## Decision

Consume Kooima's **eye positions** (the easy, convention-free output) and rebuild the projection matrix in UE's own convention via `CalculateOffAxisProjectionMatrix()` in `DisplayXRStereoMath.h`.

The matrix is UE-native reverse-Z infinite-far-plane, matching what `FMinimalViewInfo::CalculateProjectionMatrix` would produce for an on-axis camera — just with the frustum offsets derived from the tracked eye position. Concretely:

```cpp
// Inputs: ViewportHalfSize (UE units, convergence plane half-extent)
//         EyeLocation in UE local coords relative to screen center
// Output: UE reverse-Z projection matrix, infinite far plane
return AdjustProjectionMatrixForRHI(...);
```

Eye positions pass through `OpenXRPositionToUE(const XrVector3f&)` and `OpenXROrientationToUE(const XrQuaternionf&)` helpers in `DisplayXRStereoMath.h` to handle the axis swap (OpenXR +Z toward viewer → UE −X backward, OpenXR +X/+Y → UE +Y/+Z).

## Alternatives considered

1. **Use Kooima's `projection_matrix[16]` directly.** Produced visible depth/reflection artifacts. Every workaround we tried broke something else.
2. **Convert Kooima matrix to UE convention at load time.** Converting a 4×4 matrix between coordinate systems is straightforward *if both sides agree on what the matrix represents semantically* — but UE's `FViewInfo` wants a matrix that plays nicely with reverse-Z, TAA jitter, and infinite-far-plane assumptions. We'd have been re-deriving UE's projection in a less auditable way.
3. **Write a shared engine-agnostic off-axis helper.** Possible but extra moving parts for no gain — Unreal and Unity already have mature per-engine projection builders, and axis/unit conversion happens at the boundary anyway.

## Consequences

- Only **eye positions** cross the Kooima boundary. Handedness, units, and Z-range convention live inside the engine binding, not the shared math.
- `DisplayXRStereoMath.h` owns the UE-side projection. Any change to UE's projection convention (e.g. if Epic changes reverse-Z defaults) touches one helper.
- Keeps the Kooima C files engine-agnostic so they stay in sync with Unity's copy.
- Slightly more code on the UE side (rebuilding the matrix) vs. just taking Kooima's output — acceptable for the robustness gain.

## Related

- `Source/DisplayXRCore/Private/DisplayXRStereoMath.h` — the matrix + eye-offset helpers
- `Source/DisplayXRCore/Private/Native/camera3d_view.c` / `display3d_view.c` — Kooima source (shared with Unity)
- [EyeTracking.md](../EyeTracking.md) — the full `xrLocateViews` → Kooima → UE pipeline
