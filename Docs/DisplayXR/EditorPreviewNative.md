# Editor Preview: Native XR Path (Option 3)

## Context

The current editor preview (merged via PR #85, branch `editor-preview`)
uses `USceneCaptureComponent2D` + a standalone OpenXR session to render
the PIE world into a native 3D window. It works, but has inherent
limitations:

1. **No UMG / HUD capture** — `SceneCapture2D` doesn't render UI widgets
2. **Duplicate render path** — the main PIE viewport renders once, the
   preview captures again, so state (TAA history, eye-adaptation,
   post-process) has to be manually coerced to match
3. **Not zero-copy** — intermediate render targets + `CopyTextureRegion`
   to the swapchain

Game mode already solves all three via the custom HMD / compositor path
in `DisplayXRCore` — UE renders directly into the OpenXR swapchain
wrapped as an RHI texture. This plan is about making **PIE** use the
same path, eliminating the SceneCapture workaround.

## Why this failed before

From `Docs/Internal/handoff-editor-preview.md`:

> UE5.7's PIE rendering path does NOT call
> `FDisplayXRDevice::UpdateViewport()`. The XR device's viewport
> management functions are only called in standalone game mode, not in
> the editor's PIE. The compositor never initializes because
> `UpdateViewport` is never invoked.

This is the single load-bearing claim to re-verify. Every UE XR plugin
(OpenXR, SteamVR, Meta, etc.) works in PIE — so there **is** a hook
pattern that activates an XR device for PIE rendering. We either
missed it or `FDisplayXRDevice` isn't registered in a way UE recognizes
for PIE.

## Goal

Make `FDisplayXRDevice` render PIE viewport output directly into the
DisplayXR OpenXR swapchain, matching game-mode behavior. When this
works:

- PIE viewport in the editor renders stereo through our compositor
- UMG / HUD widgets are included (they render into the main viewport)
- The native 3D preview window simply *mirrors* the swapchain (or goes
  away entirely — the runtime outputs directly to the 3D display)
- No SceneCapture, no copy, no POV reconstruction

## Investigation plan

### Phase 1 — Verify the failure is still a failure

Instrument `FDisplayXRDevice` to log when each of these fires, with
timestamp + world context (editor / PIE):

- `UpdateViewport(bUseSeparateRT, FViewport&)` — the original claimed-not-called method
- `EnableStereo(bool)` / `IsStereoEnabled()` — UE queries these to decide XR mode
- `ShouldUseSeparateRenderTarget()` — gates whether UE wants an XR RT
- `GetIdealRenderTargetSize()` — called when UE sizes the XR RT
- `AllocateRenderTargetTexture(...)` / `AllocateDepthTexture(...)` — where we wrap the swapchain
- `BeginRenderViewFamily` / `SetupViewFamily` / `PreRenderViewFamily_RenderThread` — the `FSceneViewExtensionBase` callbacks

Run the editor, open a level with a rig, press Play. Compare the log
sequence to what a **working** UE XR plugin does — easiest comparator
is Epic's `OpenXRHMD` (engine plugin `Engine/Plugins/Runtime/OpenXR/`).

The point is: build ground truth about what UE actually calls in PIE
on our device, vs what we need it to call.

### Phase 2 — Read OpenXRHMD source for the PIE hook pattern

UE 5.7's `OpenXRHMD` works in PIE. Relevant code to study:

- `Engine/Plugins/Runtime/OpenXR/Source/OpenXRHMD/Private/OpenXRHMD.cpp` — session lifecycle, `OnBeginPlay`, stereo activation
- `Engine/Source/Runtime/Engine/Private/GameViewportClient.cpp` — look for where `IStereoRendering` / `IHeadMountedDisplay` methods fire during PIE draw
- `Engine/Source/Runtime/HeadMountedDisplay/` — the base classes we extend

Key questions:
1. At what point in PIE startup does UE activate stereo on the HMD?
   Is it a call we're missing (e.g., `UGameUserSettings::EnableHMD`)?
2. Does `FOpenXRHMD` register itself differently than we do (e.g.,
   `GEngine->XRSystem`, `IModularFeatures`, priority)?
3. Does PIE need `GEngine->XRSystem` to be set via
   `IHeadMountedDisplayModule::CreateTrackingSystem` on PIE begin, or
   is it a one-time registration at module load?

### Phase 3 — Match the hook pattern

Based on Phase 2 findings, adjust `FDisplayXRDevice` registration /
activation so UE drives it through the PIE render path. Candidate
changes:

- Register as `IModularFeatures` with the right feature name / priority
- Ensure `IsStereoEnabled()` returns true once the plugin module starts
  (not only after `EnableStereo(true)` is called)
- Implement `OnStartGameFrame` / `OnBeginPlay` to engage the compositor
  at PIE begin
- Handle `FWorldDelegates::OnWorldBeginPlay` to spin up per-world state

### Phase 4 — Wire PIE viewport → compositor

Once UE calls our XR path in PIE:

- `AllocateRenderTargetTexture` wraps the DisplayXR swapchain image as
  a `PF_B8G8R8A8` RHI texture (same pattern as
  `DisplayXRCompositor.cpp:287-300`)
- `FSceneViewExtensionBase` callbacks apply Kooima projection via
  `ComputeViews` (already implemented for game mode in
  `DisplayXRDevice.cpp:500-658`)
- `xrEndFrame` submits the rendered image — runtime handles interlace
  and outputs to the 3D display directly

### Phase 5 — Reconcile with the current editor preview

Once PIE rendering works natively:

- The native preview window may become redundant (the display itself is
  the preview). Delete `FDisplayXRPreviewSession` entirely.
- Or keep a small secondary window that mirrors the swapchain for cases
  where the 3D display isn't the primary monitor. Simpler than the
  current SceneCapture path.
- Update handoff docs; close the "SceneCapture vs native" tension.

## Risks

- **Failure case**: if Phase 2 reveals UE 5.7 genuinely requires an
  `FOpenXRHMD`-managed session for PIE XR, our custom device may not
  fit. Fallback: keep the SceneCapture approach as shipped.
- **Regression risk**: touching `FDisplayXRDevice` registration can
  break game mode. Build and test standalone first after every
  meaningful change.
- **Time sink**: this is a research-heavy task with uncertain
  outcomes. Set a timebox (e.g., 1–2 days of exploration) — if
  Phase 1/2 don't yield a clear path, pause and re-evaluate.

## Not doing (out of scope)

- Replacing `FDisplayXRDevice` with a thin `FOpenXRHMD` extension.
  That's a different architecture; don't mix with this investigation.
- Mac / Android support.

## Critical files

### Existing — reference only (don't modify until Phase 4)
- `Source/DisplayXRCore/Private/Rendering/DisplayXRDevice.cpp` —
  current custom HMD device (game-mode path)
- `Source/DisplayXRCore/Private/DisplayXRCompositor.cpp:287-300` —
  zero-copy swapchain wrapping pattern
- `Source/DisplayXRCore/Private/DisplayXRSession.cpp` — OpenXR session
  lifecycle
- `Source/DisplayXREditor/Private/DisplayXRPreviewSession.cpp` — the
  current SceneCapture-based editor preview (to be removed in Phase 5)

### Engine reference (read-only, for Phase 2)
- `%UE_ROOT%/Engine/Plugins/Runtime/OpenXR/Source/OpenXRHMD/Private/OpenXRHMD.cpp`
- `%UE_ROOT%/Engine/Source/Runtime/Engine/Private/GameViewportClient.cpp`
- `%UE_ROOT%/Engine/Source/Runtime/HeadMountedDisplay/Public/IHeadMountedDisplay.h`
- `%UE_ROOT%/Engine/Source/Runtime/HeadMountedDisplay/Public/IXRTrackingSystem.h`

## Verification

After Phase 4 implementation:

1. Open DisplayXRTest project in UE 5.7 editor
2. Place rig pawn in level, possess on BeginPlay
3. Press Play → editor PIE viewport should render stereo through our
   compositor. The 3D display should show correct stereo output.
4. UMG widgets added to the level should appear in both PIE viewport
   and on the 3D display.
5. Standalone game mode (`-game`) still renders correctly (regression
   check).
6. Second Play cycle works without restart.
