# Agent Prompt — Editor Preview Native XR Path

Copy-paste the block below into a fresh Claude Code session to kick
off Phase 1 of the investigation.

---

You're picking up a research + implementation task in the
`DisplayXR/displayxr-unreal` repository. Your working directory should
be a local clone of this repo.

## Your job

Read [`Docs/DisplayXR/EditorPreviewNative.md`](./EditorPreviewNative.md) first — that's the plan.
Then execute Phase 1 (instrument `FDisplayXRDevice` and log which of
its methods UE actually calls during PIE startup vs game-mode
startup). Report findings before starting Phase 2.

## What's already shipped

The current editor preview is `SceneCapture2D`-based and lives in
`Source/DisplayXREditor/Private/DisplayXRPreviewSession.*`. It works.
See [`Docs/DisplayXR/EditorPreview.md`](./EditorPreview.md) for the
current approach. This task is about replacing it with a native XR
render path, not fixing the SceneCapture one.

## The load-bearing claim to re-verify

Prior investigation concluded that `FDisplayXRDevice::UpdateViewport`
is never called in PIE — which blocked the direct compositor approach.
But Epic's `OpenXRHMD` plugin works in PIE on UE 5.5+, so there's a
hook pattern we didn't discover before. Phase 1 + Phase 2 in the plan
are specifically about finding that pattern.

## What NOT to do yet

- Don't modify game-mode rendering code (`DisplayXRCompositor`,
  `DisplayXRDevice.cpp` beyond adding logs). Game mode works; don't
  regress it.
- Don't delete the current editor preview (`DisplayXRPreviewSession`
  etc.) until Phase 4 is proven. It's the fallback.
- Don't rewrite `FDisplayXRDevice` as a thin `FOpenXRHMD` extension.
  That's a different architecture — stay with the custom HMD path
  and find the PIE activation hook.

## Constraints

- UE 5.5+, Windows, DX12 (primary target for this task)
- Mac / Android out of scope for the native-PIE investigation
- DisplayXR naming only — the repo is vendor-neutral
- Build path: install the plugin into a UE test project's
  `Plugins/DisplayXR/` folder, then build via the usual UE workflow
  (Visual Studio solution or
  `<UE_ROOT>/Engine/Build/BatchFiles/Build.bat <Project>Editor Win64 Development -Project=<path-to-.uproject> -WaitMutex -FromMsBuild`).
  Kill any running `UnrealEditor` / `LiveCodingConsole` before
  rebuilding.
- Use the UE project's `Saved/Logs/<ProjectName>.log` for verification
  — read it with the Grep tool.

## First concrete step

Open `Source/DisplayXRCore/Private/Rendering/DisplayXRDevice.cpp`
and `Source/DisplayXRCore/Private/Rendering/DisplayXRDevice.h`.
Add a one-line `UE_LOG` at the top of each of these methods, with
enough context to tell editor-from-PIE apart (include
`GEditor->PlayWorld != nullptr` or equivalent as a flag in the log):

- `UpdateViewport(...)`
- `EnableStereo(bool)` / `IsStereoEnabled()` (log enable/disable state
  transitions only — these run every frame)
- `ShouldUseSeparateRenderTarget()` (log once per world context
  change — also every-frame)
- `GetIdealRenderTargetSize()`
- `AllocateRenderTargetTexture(...)` / `AllocateDepthTexture(...)`
- `BeginRenderViewFamily` / `SetupViewFamily` /
  `PreRenderViewFamily_RenderThread`
- `OnBeginPlay(FWorldContext&)`

Then build, launch the editor, press Play, and collect the log. Share
the sequence you see — that tells us which hooks fire and which
don't, which is the whole point of Phase 1.

## Reference

- Plan: [`Docs/DisplayXR/EditorPreviewNative.md`](./EditorPreviewNative.md)
- Current preview (to be replaced): [`Docs/DisplayXR/EditorPreview.md`](./EditorPreview.md)
- Atlas handoff (zero-copy pipeline the game path already uses): [`Docs/DisplayXR/AtlasHandoff.md`](./AtlasHandoff.md)
- Eye-tracking / Kooima pipeline: [`Docs/DisplayXR/EyeTracking.md`](./EyeTracking.md)
- Epic source (read-only): `%UE_ROOT%/Engine/Plugins/Runtime/OpenXR/Source/OpenXRHMD/Private/OpenXRHMD.cpp`

Ask clarifying questions if anything about the plan or setup is
ambiguous.
