# Editor Preview System

## Key Difference from Unity

In Unity, the editor preview requires a **standalone OpenXR session** (`displayxr_standalone_*` API) because Unity's XR subsystem only creates the OpenXR session when entering Play Mode. Without PIE, there is no `XrInstance` or `XrSession` — so the Unity plugin must create and manage its own, completely independent of Unity's XR loader.

**Unreal is different.** Unreal's `FOpenXRHMD` creates the `XrInstance` and `XrSession` **at engine startup**, not at PIE entry. This means:

1. The OpenXR session with the DisplayXR runtime **already exists in the editor**
2. Our `xrLocateViews` hook **already fires** whenever the OpenXR subsystem ticks
3. Eye tracking data is **already flowing** to the plugin via the double-buffered `EyeDataBuffer`
4. The runtime is **already connected** to the 3D display

This eliminates the need for a standalone session. We can use Unreal's existing OpenXR session for editor preview.

## Recommended Approach: Use Unreal's OpenXR Session

### Strategy

Instead of creating a second OpenXR session, leverage the one `FOpenXRHMD` already manages:

1. **Eye positions**: Already available via our `xrLocateViews` hook — accessible through `UDisplayXRFunctionLibrary::GetUserPosition()` even in edit mode
2. **Stereo rendering in editor**: Enable stereo in the editor viewport via `FSceneViewExtensionBase` or by activating `FOpenXRHMD`'s stereo rendering path
3. **Frame submission**: `FOpenXRHMD` calls `xrEndFrame` as part of its render loop — if stereo is enabled, the runtime receives frames and handles interlacing

### Implementation Path (Try in Order)

**Step 1 — Verify the session exists in editor:**
Build the plugin, place in a UE 5.3+ project. Check logs for `"DisplayXR: Session created"` and `"DisplayXR: Display X.XXX x X.XXX m"` at editor startup (not PIE).

**Step 2 — Check if stereo rendering can be activated in editor:**
`FOpenXRHMD` may already support editor stereo if `IsStereoEnabled()` returns true. Check if `FOpenXRHMD` renders stereo in the editor viewport; if not, HMD plugin priority may need to be raised.

**Step 3 — If stereo isn't automatic, use FSceneViewExtensionBase:**
Create an editor view extension that:
- Queries eye positions from the extension plugin
- Applies Kooima projection to the editor viewport camera
- This gives a center-eye 3D-aware view in the editor

**Step 4 — For full stereo preview, use SceneCapture:**
If full left/right eye preview is needed (beyond center-eye):
- Create two `USceneCaptureComponent2D` with Kooima-derived transforms
- Capture to render targets each frame
- Submit through the existing session's swapchain
- Display composited output via shared texture in a Slate window

### Fallback: Standalone Session

If Step 1 reveals that `FOpenXRHMD` does NOT create the session at startup (e.g., it defers until stereo is enabled, or it doesn't tick in editor mode), then fall back to the Unity approach:

- Create a standalone `XrInstance` + `XrSession` in `FDisplayXRPreviewSession`
- Link the OpenXR loader directly (separate from Unreal's)
- Manage the full frame loop independently

This is the approach implemented in Unity — see "Reference: Unity Implementation" below.

## Preview Window UI

Menu: **Window > DisplayXR Preview**

Controls:
- **Start/Stop button** — Enables/disables editor stereo preview
- **Camera dropdown** — Lists all `UDisplayXRCamera` and `UDisplayXRDisplay` rigs in the scene
- **Rendering mode selector** — Enumerates modes from runtime via `xrEnumerateDisplayRenderingModesEXT`
- **Status footer** — Resolution, display dimensions, tracking status, current mode
- **Shared texture display** — Shows composited 3D output from the runtime

Hotkeys:
- **V** — Cycle rendering mode
- **0–8** — Select rendering mode directly
- **Tab** — Cycle active rig (via `FDisplayXRRigManager`)

## During PIE (Play Mode)

During PIE, Unreal's `FOpenXRHMD` renders stereo normally:
- Our `xrLocateViews` hook applies Kooima projection
- The runtime receives stereo frames via `xrEndFrame`
- The display processor interlaces and outputs to the 3D display
- The Game Viewport shows the normal camera view

No special overlay or camera suppression needed — the OpenXR pipeline handles everything.

## Current Implementation Status

### Implemented
- `SDisplayXRPreviewWindow` — Slate UI with Start/Stop, status display
- `FDisplayXREditorModule` — Menu registration under Window menu
- `FDisplayXRPreviewSession` — Class structure (currently stubbed for standalone approach)

### Needs Rework
- `FDisplayXRPreviewSession` — May be simplified or replaced depending on whether Unreal's session works in editor (see "Implementation Path" above)
- Shared texture display in preview window

### To Be Determined After First Build
- Whether `FOpenXRHMD` ticks and calls `xrLocateViews` in the editor
- Whether editor stereo rendering can be activated
- Whether a `FSceneViewExtensionBase` is sufficient for center-eye preview

## Reference: Unity Implementation

The Unity plugin **requires** a standalone session because Unity's XR subsystem only initializes during Play Mode.

Key files in `dfattal/unity-3d-display`:
- `Editor/DisplayXRPreviewSession.cs` (~500 lines) — Creates independent `XrInstance`/`XrSession` via `displayxr_standalone_*()` P/Invoke calls
- `Editor/DisplayXRPreviewWindow.cs` (~400 lines) — Editor window with camera dropdown, mode selector, auto-refresh
- `Runtime/DisplayXRGameViewOverlay.cs` (~340 lines) — Renders shared texture in Game View during Play Mode, suppresses camera rendering
- `native~/displayxr_standalone.h/.cpp` — Native C/C++ standalone session API

Unity's approach during Play Mode:
1. Disables Unity's XR loader (`PlayerSettings.SetPlatformXREnabled = false` via SessionState)
2. The standalone preview session continues rendering via SceneCapture
3. `DisplayXRGameViewOverlay` displays the shared texture in Game View
4. On exiting Play Mode, re-enables Unity's XR loader

**This complexity is unnecessary in Unreal** because `FOpenXRHMD` manages the session across the entire editor lifecycle, not just during gameplay.
