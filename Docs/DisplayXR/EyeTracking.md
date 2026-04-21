# Eye Tracking & Parallax — DisplayXR UE Plugin

## Overview

The DisplayXR plugin uses the OpenXR runtime's face tracking to provide
head-tracked parallax. Eye positions from `xrLocateViews` feed into the
Kooima C library (`display3d_compute_views`) which computes per-view
off-axis projection matrices and camera offsets.

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

### 3. Display-Centric View Offset Uses eye_world

For display-centric mode (the default), the camera offset and projection
matrix both use `eye_display` from the Kooima library, converted to UE
coordinates via `OpenXRPositionToUE()`.

- **Camera offset** (`CalculateStereoViewOffset`): Full `eye_display` XYZ.
  The camera moves to the eye position — this is correct and matches the
  Unity/test app implementations which set `view.pose.position = eye_world`.

- **Projection matrix** (`CalculateOffAxisProjectionMatrix`): Same `eye_display`
  position, which `ToScreenSpace()` converts to screen-local coords for the
  asymmetric Kooima frustum.

- **Screen half-size**: `screen_meters * m2v * 100` where `m2v = virtual_display_height / screen_height_m`.
  This matches Kooima's `kScreenW/kScreenH` scaling (m2v only, no perspective_factor —
  perspective_factor only applies to eye position).

**File**: `DisplayXRDevice.cpp`, `ComputeViews()` display-centric branch.

## Coordinate Conventions

| Space | X | Y | Z | Units |
|-------|---|---|---|-------|
| OpenXR display-local | Right | Up | Toward viewer | Meters |
| Kooima (same as OpenXR) | Right | Up | Toward viewer | Virtual units (meters × m2v) |
| UE world | Forward (into screen) | Right | Up | Centimeters |
| UE screen-local (`ToScreenSpace`) | Right | Up | Toward viewer | Centimeters |

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
