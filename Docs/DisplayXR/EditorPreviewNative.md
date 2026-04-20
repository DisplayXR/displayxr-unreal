# Editor Preview: Native XR Path (Option 3)

## Status (2026-04-20)

**Phase 1–3 landed** behind `r.DisplayXR.EditorNativePIE 1` (commit
`40e9265` on branch `editor-preview-xr-native`). With the CVar on:

- UE's stereo pipeline drives PIE rendering natively through
  `FDisplayXRDevice` — same code path as standalone game mode.
- The atlas is rendered zero-copy into our OpenXR swapchain. UMG, HUD,
  post-process, TAA, Lumen, Niagara are all included for free.
- The atlas is visible in the editor PIE viewport tab (pre-weave).
- No `SceneCapture2D`, no duplicate render pass, no texture copies
  from the render path.

**Phase 4 is open** — routing the atlas to a visible top-level window
on the 3D display from editor PIE. The WIP exploration is preserved at
git tag `wip/mirror-swapchain-exploration` (commit `882b3ae`) but is
**not** the path we're taking in the next session; see Phase 4 plan
below for the new approach.

## Context

Editor PIE does not fullscreen onto the 3D display the way standalone
game mode does. When running PIE with the native XR path active, UE
renders the stereo atlas correctly, but the atlas never reaches a
top-level visible window on the 3D display. Instead the display shows
the weaved editor desktop (whatever pixels are physically visible get
weaved by the 3D display hardware).

Game mode doesn't hit this because the game window is a top-level
UE-managed `SWindow`. In gameplay mode, the `DisplayXR` runtime's D3D12
native compositor binds to that `SWindow`'s HWND and presents cleanly
through DWM.

## Key insight — windowed game works

Test result (2026-04-20): running the gameplay target with `-Windowed`
at 3840×2160 on the 3D display **still shows the correct atlas**.
Gameplay is not special because it's fullscreen; it's special because
the window is UE-managed.

This rules out "fullscreen takeover" as a requirement, and points
squarely at **raw Win32 `CreateWindowExW` vs UE `SWindow`** as the
culprit for the editor-PIE mirror window we tried (the WIP).

## Load-bearing claim (verified in Phase 1)

From `Docs/Internal/handoff-editor-preview.md`:

> UE 5.7's PIE rendering path does NOT call
> `FDisplayXRDevice::UpdateViewport()`. The compositor never
> initializes because `UpdateViewport` is never invoked.

**Verified and remediated.** Root cause:
`UEngine::IsStereoscopic3D` (`UnrealEngine.cpp:4521`) requires the
viewport to return true from `IsStereoRenderingAllowed()`, which comes
from `SViewport::bEnableStereoRendering`, which `PlayLevel.cpp:3377`
only sets when `bVRPreview` is true. Fix: we call
`SViewport::EnableStereoRendering(true)` on the PIE viewport in
`OnPostPIEStarted` when the CVar is on. `FXRRenderTargetManager`
methods then fire end-to-end in PIE.

## Goal

Route the atlas already being produced by Phase 1–3 to a visible
top-level window on the 3D display, from editor PIE. When done, the
native path replaces the shipped `SceneCapture2D` preview.

## Phase 4 — UE-managed mirror window (next session)

The WIP in `wip/mirror-swapchain-exploration` used raw
`CreateWindowExW`. Symptoms: mirror visible as TOPMOST foreground, but
its client area shows the editor through; our own DXGI swapchain
attempts crashed with `DXGI_ERROR_DEVICE_REMOVED`.

**New approach: create the mirror as a UE `SWindow`** via
`FSlateApplication::Get().AddWindow(SNew(SWindow)...)`. UE owns the
swapchain, DWM registration, DPI handling, focus, message pumping — all
the machinery that windowed gameplay gets "for free" and that we
poorly replicated with raw Win32.

Steps:

1. In `FDisplayXREditorModule::OnBeginPIE`, when CVar is on: create a
   top-level `SWindow` sized to the DisplayXR display dimensions.
   Attributes: no initial focus (`.CreatedDuringInitialization(false)`
   / don't steal foreground), non-topmost to match windowed game
   behavior, or minimally topmost if needed for visibility.
2. Retrieve the native HWND via
   `Window->GetNativeWindow()->GetOSWindowHandle()`.
3. Set `FDisplayXRPlatform::OverrideCompositorHWND` to that HWND so
   `FDisplayXRDevice::UpdateViewport` binds the OpenXR session to it
   (wiring already in place from Phase 3b WIP).
4. On `OnPrePIEEnded`, clear the override and close the `SWindow`.
5. Skip `CreateChildWindow` in `FDisplayXRCompositor::Initialize` when
   `OverrideCompositorHWND` is set (already done in
   `DisplayXRCompositor.cpp:485-510`).
6. Keep `xrSetSharedTextureOutputRectEXT` wired (already done in the
   WIP; per-frame call is required for the runtime's D3D12 native
   compositor to route swapchain present correctly).

If the approach works, the runtime's D3D12 native compositor should
present the atlas into the `SWindow`'s client area, matching the
windowed-game code path exactly.

### What to cherry-pick from the WIP tag

From `wip/mirror-swapchain-exploration`, the parts we keep:

- `DisplayXRCompositor.cpp` changes: `xrSetSharedTextureOutputRectEXT`
  resolution + per-tick call; conditional child-window skip when
  `OverrideCompositorHWND` is set.
- `DisplayXRPlatform.h`: `OverrideCompositorHWND` hook.
- `DisplayXREditorModule.cpp`: CVar, `OnPostPIEStarted` SViewport flip
  (already landed as Phase 3 base — `40e9265`).

What we drop:

- Raw `CreateWindowExW` mirror window class + WndProc.
- The `MirrorPresent` function-pointer hook + DXGI swapchain + manual
  atlas copy (crashed with DEVICE_REMOVED; not needed if the runtime
  compositor presents correctly to the `SWindow`).
- Per-tick `SetWindowPos(HWND_TOPMOST)` ticker.

## Phase 5 — Dev QoL + cleanup

Bundle with Phase 4:

1. **SHIFT+F1 dev shortcut** (release/capture mouse in game builds).
   Currently in `DisplayXRCoreModule.cpp` but not firing — Slate
   isn't initialized at `PostConfigInit` module load. Defer
   registration via `FDelayedAutoRegisterFunction` or
   `FTSTicker` until `FSlateApplication::IsInitialized()`. Add a log
   when the key event is handled so we can confirm it fires.
2. **Remove Phase 1 instrumentation logs** from
   `DisplayXRDevice.cpp` — `WorldCtxTag`, `WorldTypeStr`, the
   per-method first-call / every-300 logs, the `SetupViewPoint`
   pawn-name diagnostic. All useful during investigation, now noise.
3. **Remove the shipped `SceneCapture`-based preview** once the
   native path is proven stable on the 3D display output side. Delete
   `FDisplayXRPreviewSession`, `DisplayXRPreviewWindow.*` (already
   skeletal), SceneCapture-specific code in the editor module. Flip
   the CVar default to `1` (or remove the CVar entirely) once the
   native path is the only path.
4. Update `Docs/Internal/handoff-editor-preview*.md` — close the
   SceneCapture-vs-native tension, point to the native path as
   ship-state.

## Risks

- **The `SWindow` might not fix it.** If DWM/D3D12 compositing on a
  UE `SWindow` still doesn't route the runtime's present visibly, the
  fallback is either (a) do the mirror-swapchain + manual copy
  approach properly (fix the DEVICE_REMOVED from WIP by using
  `AddCopyTexturePass` so RDG owns transitions + moving Present to a
  deferred game-thread dispatch), or (b) keep the SceneCapture path
  alive as the editor-PIE output mechanism while the native path
  handles only the editor-tab preview.
- **Per-project input bindings.** Users who don't want SHIFT+F1 can't
  trivially disable it. Low risk — it's a rare keycombo — but consider
  a CVar to disable if complaints land.
- **Two Play cycles in one editor session** still need stress
  testing — our Phase 3 commits haven't been exercised across
  back-to-back PIE start/stop cycles with teardown.

## Not doing (out of scope)

- Replacing `FDisplayXRDevice` with a thin `FOpenXRHMD` extension.
  Different architecture — `OpenXRHMD` is HMD-only, lacks N-view atlas
  support, off-axis projection, display-mode switching. Our custom
  stack is architecturally correct for DisplayXR.
- Mac / Android support.
- Picture-in-picture cropped-tile preview in the editor tab. Nice to
  have — shipped SceneCapture preview doesn't do this either. Track
  separately.

## Critical files

### Landed (Phase 1–3)
- `Source/DisplayXRCore/Private/Rendering/DisplayXRDevice.cpp` —
  instrumentation + `OnBeginPlay` + `SetupViewPoint` diagnostics.
- `Source/DisplayXREditor/Private/DisplayXREditorModule.cpp` —
  `r.DisplayXR.EditorNativePIE` CVar + PIE lifecycle hooks
  (`OnBeginPIE`, `OnPostPIEStarted`, `OnPrePIEEnded`).
- `Source/DisplayXRCore/Private/DisplayXRCoreModule.cpp` — SHIFT+F1
  preprocessor (broken registration; Phase 5 item 1).

### WIP reference (tag `wip/mirror-swapchain-exploration`)
- `Source/DisplayXRCore/Private/DisplayXRPlatform.h` —
  `OverrideCompositorHWND`, `MirrorPresent` function-pointer hook.
- `Source/DisplayXRCore/Private/DisplayXRCompositor.cpp` —
  `xrSetSharedTextureOutputRectEXT` resolution + per-tick call;
  conditional child-window skip.
- `Source/DisplayXREditor/Private/DisplayXREditorModule.cpp` (at
  tagged commit) — raw-Win32 mirror window, DXGI swapchain attempt,
  manual atlas copy.

### Engine reference (read-only, for Phase 4)
- `%UE_ROOT%/Engine/Source/Runtime/SlateCore/Public/Widgets/SWindow.h`
- `%UE_ROOT%/Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h`
  — `AddWindow` contract.
- `%UE_ROOT%/Engine/Plugins/Runtime/OpenXR/Source/OpenXRHMD/Private/OpenXRHMD.cpp`
  — reference for how Epic binds its compositor to window swapchains.

## Verification

After Phase 4:

1. Open `DisplayXRTest` in UE 5.7 editor.
2. Console: `r.DisplayXR.EditorNativePIE 1`.
3. Press Play (any mode — Selected Viewport, New Editor Window).
4. A top-level `SWindow` titled "DisplayXR Preview (native)" appears.
   Its client area shows the **weaved atlas** (correct rig view) on
   the 3D display.
5. Editor PIE viewport tab shows the same atlas pre-weave.
6. Stop PIE — the `SWindow` closes cleanly, editor desktop
   re-appears 2D on the 3D display.
7. Start a second PIE cycle — window re-creates cleanly, atlas
   correct.
8. Run `run-game.bat` (windowed or fullscreen) — still renders
   correctly (regression check).
9. SHIFT+F1 in game mode → mouse cursor appears and lookaround stops;
   SHIFT+F1 again → mouse re-captured.

## References

- Branch: `editor-preview-xr-native`
- Phase 3 stable commit: `40e9265`
- WIP Phase 4 exploration: tag `wip/mirror-swapchain-exploration`
  (commit `882b3ae`)
- Agent prompt for next session:
  `Docs/DisplayXR/EditorPreviewNative-AgentPrompt.md`
- Prior handoffs: `Docs/Internal/handoff-editor-preview.md`,
  `handoff-editor-preview-pie-integration.md`,
  `handoff-ue57-custom-hmd.md`
