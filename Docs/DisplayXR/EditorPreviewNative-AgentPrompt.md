# Agent Prompt — Editor Preview Native XR Path (Option 3)

Copy-paste the block below into a fresh Claude Code session to kick
off Phase 1 of the investigation.

---

You're picking up a research + implementation task on the
`editor-preview-xr-native` branch of the `displayxr-unreal` repo at
`C:\Users\Sparks i7 3080\Documents\GitHub\displayxr-unreal`.

## Your job

Read `Docs/DisplayXR/EditorPreviewNative.md` first — that's the plan.
Then execute Phase 1 (instrument `FDisplayXRDevice` and log which of
its methods UE actually calls during PIE startup vs game-mode
startup). Report findings before starting Phase 2.

## What's already shipped

PR #85 (`editor-preview` → `main`) delivered a working but
SceneCapture-based editor preview. Full journey documented at
`Docs/Internal/handoff-editor-preview-pie-integration.md`. The
shipped approach works; this task is about replacing it with a
native XR render path.

## The load-bearing claim to re-verify

`Docs/Internal/handoff-editor-preview.md` states that
`FDisplayXRDevice::UpdateViewport` is never called in PIE — which
blocked the direct compositor approach. But Epic's `OpenXRHMD`
plugin works in PIE on UE 5.7, so there's a hook pattern we didn't
discover before. Phase 1 + Phase 2 in the plan are specifically
about finding that pattern.

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

- UE 5.7, Windows, DX12 only
- DisplayXR naming — no vendor branding anywhere in module / symbol names
- Build path: copy modified sources to
  `C:/Users/Sparks i7 3080/Documents/Unreal Projects/DisplayXRTest/Plugins/DisplayXR/Source/`,
  kill `UnrealEditor` + `LiveCodingConsole`, run
  `UE_5.7/Engine/Build/BatchFiles/Build.bat DisplayXRTestEditor Win64 Development -Project=... -WaitMutex -FromMsBuild`.
- Log to the `DisplayXRTest.log` under `Saved/Logs/` in the test
  project — read it with the Grep tool.

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

- Plan: `Docs/DisplayXR/EditorPreviewNative.md`
- Architecture docs: `Docs/Internal/Backends.md`, `Docs/Internal/Modules.md`
- Prior handoffs (read for context, not instruction):
  `Docs/Internal/handoff-ue57-custom-hmd.md`,
  `Docs/Internal/handoff-editor-preview.md`,
  `Docs/Internal/handoff-editor-preview-pie-integration.md`
- Epic source (read-only): `%UE_ROOT%/Engine/Plugins/Runtime/OpenXR/Source/OpenXRHMD/Private/OpenXRHMD.cpp`

Ask me clarifying questions if anything about the plan or setup is
ambiguous.
