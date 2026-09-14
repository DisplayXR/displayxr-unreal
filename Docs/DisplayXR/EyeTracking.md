# Eye Tracking & Parallax — DisplayXR UE Plugin

## Overview

The DisplayXR plugin uses the OpenXR runtime's face tracking to provide
head-tracked parallax. The plugin chains an `XR_DXR_view_rig` descriptor onto
`xrLocateViews`, so the **runtime** applies the tracked eyes to the rig and
returns render-ready `XrView{pose, fov}` — per-view off-axis frustum and camera
offset — which the plugin converts to UE reverse-Z matrices (#396 W7, ADR-024).

## Pipeline

```
Compositor Thread                  Game Thread
─────────────────                  ───────────
xrWaitFrame                        Session->Tick()
  → predictedDisplayTime             → LocateViews()
  → SetPredictedDisplayTime()          → xrLocateViews(displayTime)
                                       → Store eye positions (double-buffered)
                                     ComputeViews()
                                       → GetEyePositions()
                                       → display3d_compute_views() [Kooima]
                                       → CachedViews[].Offset (eye_world → UE)
                                       → CachedViews[].ProjectionMatrix
```

## Key Fixes

### 1. displayTime Must Come From xrWaitFrame

`xrLocateViews` requires a valid `displayTime` from the runtime. Passing 0
returns `XR_ERROR_TIME_INVALID` (-30), causing eye positions to never update.

**Fix**: The compositor thread stores `predictedDisplayTime` from `xrWaitFrame`
via `Session->SetPredictedDisplayTime()`. The game thread's `LocateViews()`
reads it via `PredictedDisplayTime.Load()`.

**Files**: `DisplayXRCompositor.cpp` (store), `DisplayXRSession.h` (atomic member),
`DisplayXRSession.cpp` (use in LocateViews).

### 2. xrPollEvent Blocks After Session Running

The in-process runtime's `xrPollEvent` blocks the game thread once the
compositor thread is active. The original 60-tick skip count caused a hard
freeze at exactly frame 60 when polling resumed.

**Fix**: Skip `xrPollEvent` entirely once `bSessionRunning=true`. The compositor
thread handles all runtime interaction via `xrWaitFrame`.

**File**: `DisplayXRSession.cpp`, `Tick()`.

### 3. View Offset and Projection Come From the Runtime

For both rig modes the plugin chains an `XrDisplayRigDXR` / `XrCameraRigDXR`
descriptor onto `xrLocateViews` (`XR_DXR_view_rig`) and consumes the
render-ready `XrView{pose, fov}` the runtime returns. No view math runs in the
plugin.

- **Camera offset** (`CalculateStereoViewOffset`): `view.pose.position`,
  converted to UE centimetres via `OpenXRPositionToUE()`. The camera moves to
  the eye position the runtime computed for the active rig.

- **Projection matrix** (`ProjectionMatrixFromFov`): `view.fov` converted to a
  UE reverse-Z projection. The fov is clip-independent, so near/far and the
  reverse-Z convention are the only app-side contributions.

- **Screen size, convergence, look-around and baseline factors** are inputs to
  the rig descriptor; the runtime applies them. The plugin never derives a
  frustum from a convergence plane and an eye position.

**File**: `DisplayXRDevice.cpp`, `ComputeViews()`; editor path in
`DisplayXRPreviewSession.cpp`.

## Coordinate Conventions

| Space | X | Y | Z | Units |
|-------|---|---|---|-------|
| OpenXR display-local | Right | Up | Toward viewer | Meters |
| Kooima (same as OpenXR) | Right | Up | Toward viewer | Virtual units (meters × m2v) |
| UE world | Forward (into screen) | Right | Up | Centimeters |

`OpenXRPositionToUE(V)`: `(-V.z × 100, V.x × 100, V.y × 100)`

## Diagnostic Logging

Periodic logs (every 300 frames) in the tracking pipeline:

- `LocateViews #N`: xrLocateViews result, viewStateFlags, raw eye positions, displayTime
- `LocateViews FAILED #N`: xrLocateViews error code, session/space handles, displayTime
- `ComputeViews #N`: tracked flag, fallback flag, raw eye L/R, computed offsets per view

## Fallback Behavior

If both eye positions are `(0,0,0)` (tracking not ready or runtime returning
zeros), `ComputeViews` falls back to static nominal viewer positions with
63mm IPD. This provides a valid default stereo view until tracking activates.

The fallback is logged as `fallback=1` in the `ComputeViews` diagnostic.
