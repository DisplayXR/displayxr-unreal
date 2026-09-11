# TODO

Outstanding work for the DisplayXR Unreal plugin. Organized by theme, not priority — grab whichever unlocks your next milestone.

---

## 1. In-flight work

### Native XR editor preview
Replace the current `SceneCapture2D`-based editor preview with a `FDisplayXRDevice` → PIE hookup so UE renders directly into the OpenXR swapchain in the editor (zero-copy, UMG/HUD supported).

- Motivation: remove duplicate render path, pick up TAA history / eye adaptation / post-process state automatically, include UI widgets in the preview.
- Phases 1–3 are in `main` behind `r.DisplayXR.EditorNativePIE 1`.
- Plan: [`EditorPreviewNative.md`](./EditorPreviewNative.md) (5-phase investigation) — but see the superseded banner at the top: **Phase 4's separate-mirror-window approach is not the path**. Agent handoff prompt: [`EditorPreviewNative-AgentPrompt.md`](./EditorPreviewNative-AgentPrompt.md).

#### Weaved preview in the PIE viewport tab ([#38](https://github.com/DisplayXR/displayxr-unreal/issues/38))
Parity with [`displayxr-unity`](https://github.com/DisplayXR/displayxr-unity) v2.8.0: the weaved preview shows up **inside the PIE viewport tab**, with floating the tab optional. Design and open questions live on issue #38.

- ✅ **M0** — `XR_DXR_display_zones` vendored, probed, enabled (`FDisplayXRSession::HasDisplayZones()`); ABI pin at `displayxr-runtime@b0e5889`.
- ✅ **M1** — compositor rebuild across PIE runs (`NotifyPlaySessionStarting` / `NotifyPlaySessionEnded`). Amended in M2: the XrSession itself is also rebound per editor play session — the compositor alone was not enough (the stale session stayed bound to the previous cycle's dead window).
- ✅ **M2** — texture-mode binding shipped and **weaved 3D confirmed in the PIE tab on hardware**: shared D3D12 texture (worst-case-sized per ADR-010) via `sharedTextureHandle`, invisible click-through proxy HWND (`FDisplayXRPIEPreview`) as the interlace-phase anchor, full-pane zone on `xrLocateViews` + the projection layer, woven blit via `OnBackBufferReadyToPresent` + XRBase `AddXRCopyTexturePass`. Editor-mode render wiring: UE renders the atlas into the viewport's OWN RT (all separate-RT hooks refused — a docked stereo separate-RT viewport flips Slate onto its stereo-composite path and the editor UI stomps the swapchain) and `PostRenderViewFamily_RenderThread` copies atlas→swapchain per frame; the PIE viewport is unset from the window's registered-viewport slot for the duration.
  - Remaining rough edges (post-M4): ~1 s of pre-weave SBS at Play while the session rebinds; 10-bit/HDR editor backbuffer colors need a PQ-aware encode pass for exactness; mixed-DPI multi-monitor layouts untested.
  - **Size the shared texture worst-case and allocate it once** — runtime ADR-010 + app-rules INV-5.2 specify `max over modes of (tileColumns × viewScaleX × displayPixelWidth)` by the same for height, never resized; a window resize changes the *zone rect*, not the surface. So resize never costs a session recreate.
  - Add that max-over-modes helper and use it for the swapchain too. `CreateSwapchain` currently uses panel dims and `QueryRenderingModes` only records the active 3D mode, so we never take the max — correct today only because SBS 2×1 @ 0.5 makes atlas == panel.
  - Read back from the origin: post-ADR-031 the canvas is `(0, 0, clientW, clientH)`, so `uvOffset` is zero and only `uvScale = (clientW/texW, clientH/texH)` matters. **The runtime validates no dimensions** — a window bigger than the texture truncates silently, so the app clamps.
  - **Always submit a zones frame, even a single full-window zone.** Without one the display processor gets `canvas_width/height = 0`, which its ABI defines as "fill the whole target" — i.e. the entire worst-case texture, magnifying the weave and destroying the interlace.
  - Format is ours to choose — the runtime builds its render-target view from our `GetDesc().Format`. Allocate BGRA (`PF_B8G8R8A8`) to match the reference apps. Keep the blit a format-converting shader draw: a `CopyTextureRegion` cannot cross the RGBA/BGRA copy groups.
- ✅ **M3** — live glue shipped: per-tick pane rect in physical px, immediate moves, 0.35 s settle-debounced resizes (zone + proxy + blit updated together), dock↔float handled by a full preview restart against the new window (no `SetParent` on the weaver-bound HWND — stereo-collapse trap). Verified live with mid-Play editor resizes; the float-the-tab path still wants a manual hardware pass.
- **M4** — fallbacks and polish: one-shot WARN when the runtime lacks display zones (tab keeps showing the pre-weave atlas), HDR10 backbuffer handling, HiDPI validation.
- ✅ **Cleanup (M4)** — `FDisplayXRPreviewSession` + SceneCapture path deleted; raw-Win32 mirror window + `r.DisplayXR.EditorNativePIEMirror` deleted (`OverrideCompositorHWND` stays — the proxy rides it); `r.DisplayXR.EditorNativePIE` default-on (0 = no editor preview); Phase-1 instrumentation stripped from `DisplayXRDevice.cpp`; dead `bSuppressCompositor` removed. The SHIFT+F1 note in the issue was stale — registration already defers to `OnPostEngineInit` and the log confirms it fires.

Known hardware traps inherited from the Unity bring-up, all documented on #38: never pass `SWP_FRAMECHANGED` to `SetWindowPos` on the weaver-bound HWND (permanently collapses stereo to mono); debounce interactive resize (swapchain-realloc storms hung the D3D12 device); push physical pixels, never logical points.

---

## 2. Parity with [displayxr-unity](https://github.com/DisplayXR/displayxr-unity)

The Unity sibling has a more mature docs tree. Bring Unreal up to parity so users can switch between plugins without surprises.

### Done
- ✅ Quick-start guide → [`QuickStart.md`](./QuickStart.md)
- ✅ Single-page architecture doc → [`Architecture.md`](./Architecture.md)
- ✅ First 3 ADRs (Unreal-specific: direct runtime loading, zero-copy atlas, UE-native off-axis projection) → [`adr/`](./adr/)
- ✅ App-manifest pipeline (cook/stage emits `<exe>.displayxr.json` + optional `%LOCALAPPDATA%\DisplayXR\apps\` registered manifest) → [issue #5](https://github.com/DisplayXR/displayxr-unreal/issues/5). Win64 only; Mac parity is open below.

### Still open

- **`CHANGELOG.md`** — Keep-a-Changelog format. Seed with `## [0.1.0]` summarizing the initial DisplayXR release.
- **Additional ADRs mapped from Unity's set** — Unity has 6 ADRs covering decisions that are likely to have Unreal-side analogues or counter-decisions. Write a matching Unreal ADR (or a "does not apply because X" stub) for each:
  - Unity ADR-001 *deferred-destruction* — Unity-specific teardown ordering; check if UE's module shutdown path has the same trap.
  - Unity ADR-002 *dual-session* — Unity runs two OpenXR sessions (game + preview). We also do this via `FDisplayXRPreviewSession`; capture the Unreal-side tradeoffs.
  - Unity ADR-003 *native-preview-window* — Relevant to current `FDisplayXRPreviewSession`; describe the Unreal window-creation path.
  - Unity ADR-004 *camera-vs-display-mode* — We ship both `UDisplayXRCamera` and `UDisplayXRDisplay`; record the selection criteria.
  - Unity ADR-005 *multipass-forced* — answered in [`InstancedStereo.md`](./InstancedStereo.md): UE decides the mode at cook time (`vr.InstancedStereo`), the device cannot force one; multipass and desktop ISR both work with the 2x1 atlas, and the plugin works around the UE 5.7 sky-light capture crash under ISR. (Unity's ADR-005 is itself superseded by its ADR-007 render-path-by-view-count.)
  - Unity ADR-006 *window-relative-kooima* — Relevant for display-centric math; record how the Unreal port handles window-rect vs monitor-rect.
- **`Docs/DisplayXR/architecture/` subfolder** (optional depth docs) — Unity has `hook-chain.md`, `kooima-pipeline.md`, `preview-session.md`. Our current [`Architecture.md`](./Architecture.md) is one page; split if it grows past that. Kooima pipeline detail already lives in [`EyeTracking.md`](./EyeTracking.md); preview-session detail is split across [`EditorPreview.md`](./EditorPreview.md) and [`EditorPreviewNative.md`](./EditorPreviewNative.md).
- **`Docs/DisplayXR/roadmap/`** — Unity has planning docs (`full-gfx-backends-refactor-plan.md`, `phase1-mac-finish.md`, etc.). Carry over the ones relevant to shared native code; add UE-specific roadmap items.
- **Top-level README polish** — current [`README.md`](../../README.md) has a TOC and the "New here?" callout. Unity's README is richer (per-section screenshots, deployment, troubleshooting). Bring the density up once we have a companion test project to link from.
- **Companion test project** — create `DisplayXR/displayxr-unreal-test` mirroring `DisplayXR/displayxr-unity-test`: a minimal UE project with this plugin preconfigured, two demo maps (camera-centric, display-centric), ready to open and hit Play.

---

## 3. CI & release

- **Real UE build in CI** — current `lint.yml` only does vendor-name + JSON/YAML checks. Full compile needs either a self-hosted runner with UE preinstalled or Epic's container registry (requires Epic account linkage). Matrix target: UE 5.7 (the version the plugin declares).
- **Release workflow** — package `DisplayXR.uplugin` per UE version, upload to GitHub Releases. Replaces any prior internal upload flow.
- **Tagged versioning** — `VersionName` in `DisplayXR.uplugin` should be kept in sync with git tags (e.g. `v0.1.0`).

---

## 4. Platform coverage

- **macOS path validation** — end-to-end smoke test of the unified `FDisplayXRSession` (Metal graphics binding, Cocoa window binding) on a supported display. See [MacSetup.md](./MacSetup.md).
- **Mac parity for app manifest** — port the Win64 manifest pipeline (issue #5) to Mac: settings panel already compiles cross-platform, but `Scripts/PackageApp.py` is Windows-only and the registered-mode path needs a Mac equivalent (likely `~/Library/Application Support/DisplayXR/apps/` — confirm against runtime spec).
- **Android path validation** — unified session on Android (Vulkan graphics binding).
- **UE version sweep** — the plugin declares EngineVersion 5.7.0 and has since the first commit; docs that claimed "5.3+" were never accurate and are now corrected. If a lower floor is wanted, lower EngineVersion and actually verify it builds — do not re-assert a range nothing tests.

---

## 5. Known issues / cleanup

- The plugin computes no view math: the runtime owns it via `XR_DXR_view_rig` (#396 W7, ADR-024). There is no `displayxr-common` submodule and no `displayxr::math` link — do not re-vendor `display3d_view.*` / `camera3d_view.*`, the `drift-guard` workflow fails on it.
- Some file-header years are `2025-2026`; new files should use `2026-` or extend the range as appropriate.
