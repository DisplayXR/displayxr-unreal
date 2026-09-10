# Changelog

All notable changes to the DisplayXR Unreal plugin are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.9.0] - 2026-09-10

### Fixed
- **2D UI (UMG/Slate) was painted once across the eye atlas.** On the game path Slate's stereo-composite step paints the whole window's UI into the viewport render target — the swapchain image holding both eye tiles — so the left eye showed the left part of the HUD and the right eye the right part, each scaled up by the weave; the UI was also painted after the image had been released. UE now renders the atlas into its own window-sized target, `PostRenderViewFamily` copies the tiles into the swapchain image and clears the target, Slate paints the UI onto that transparent layer, and `RenderTexture_RenderThread` alpha-blends it into every eye tile (screen plane) before releasing the image. Costs one tile-area copy per frame; `r.DisplayXR.UIPerEyeTiles 0` restores the zero-copy flow. Hit-testing is unchanged (window pixels in, window pixels out). The target is sized from the viewport (not the compositor's bound overlay, whose size the runtime derives from what we render — a feedback loop). See `Docs/DisplayXR/UICompositing.md`.
- **Scene / reflection captures no longer touch the swapchain hand-off.** Their view families reach the device's view extension too and used to release the frame's swapchain image prematurely (and, on the per-eye UI path, would have copied the capture into the swapchain and cleared the capture's target). `PostRenderViewFamily_RenderThread` now ignores them.

### Changed
- **The game path is no longer zero-copy by default.** UE renders the stereo atlas into its own window-sized render target and the tiles are copied into the OpenXR swapchain image once per frame so the window's 2D UI can be composited into every eye tile (`r.DisplayXR.UIPerEyeTiles`, default 1, `ECVF_ReadOnly`: set it in `DefaultEngine.ini` or with `-dpcvars`). `r.DisplayXR.UIPerEyeTiles 0` restores the v0.8.0 zero-copy flow. Editor texture mode and the IPC array-copy path are unchanged. See `Docs/DisplayXR/UICompositing.md`.

## [0.8.0] - 2026-09-09

### Added
- **Rig gizmos in the editor.** Selecting a `DisplayXR Camera` draws its convergence plane — the screen plane at `1/InvConvergenceDistance` in front of the camera, sized from the camera's FOV and aspect, with edges back to the camera; inverse distance 0 (parallel) shows a 2 m preview plane in a different hue. Selecting a `DisplayXR Display` draws the display plane at the camera transform, sized from `VirtualDisplayHeight` or the physical display the runtime reports. Implemented as `FComponentVisualizer`s registered by `DisplayXREditor`; no runtime-module change. Tuning convergence no longer needs a Play-and-look cycle. ([#47](https://github.com/DisplayXR/displayxr-unreal/issues/47))

### Fixed
- **Camera-centric rig sent the camera's horizontal FOV as `verticalFov`.** `UDisplayXRCamera` forwarded `UCameraComponent::FieldOfView` — which Unreal defines as the *horizontal* angle — straight into the rig's vertical field, so the stereo frustum did not match the 2D view (90° tall instead of 58.7° for the default camera). (The Unity sibling is correct as-is: its `Camera.fieldOfView` is vertical.) The rig now derives the vertical angle the way UE itself does — `FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle` with the local player's aspect-ratio axis constraint and the stereo canvas rect (published from `AdjustViewRect`), reading the vertical half-angle off the projection matrix — so `MaintainYFOV` / `MaintainXFOV` / `MajorAxisFOV` and constrained-aspect cameras all match their 2D framing. `FDisplayXRTunables::FovOverride` now carries the vertical angle; an orthographic camera sends 0 and gets the runtime's default vertical fov. ([#43](https://github.com/DisplayXR/displayxr-unreal/issues/43))
- **Per-camera rigs.** `UDisplayXRCamera` / `UDisplayXRDisplay` are now scene components (via the new `UDisplayXRRigComponent` base) so a rig attaches under the camera it drives; unattached rigs still bind to the owner's first camera as before. `FDisplayXRRigManager` is the single tunables pusher: once per frame (`SetupViewFamily`, before the locate) it sends the tunables of the one rig on the camera the local player renders from — the view target's first active camera component, `AActor::CalcCamera`'s rule. Previously every rig pushed every tick into the single session slot, so with several cameras on one pawn the last rig to tick won, non-deterministically. A rendered camera with no rig now resets the session to default tunables instead of inheriting the previous rig's (the session outlives PIE). ([#45](https://github.com/DisplayXR/displayxr-unreal/issues/45))
- **Docs: the display plane is the camera transform.** The `UDisplayXRDisplay` header and README described a display placed independently of the camera ("parent transform defines the display; camera is a child"); that was a mis-port. In both plugins the camera transform is the display plane and the viewer moves around it.

### Removed
- `DisplayXRComponentProxies` (`UDisplayXRCameraProxy` / `UDisplayXRDisplayProxy`): dead code — nothing instantiated them and they drew a fixed 100 cm frustum unrelated to any tunable.

### Changed
- **`UDisplayXRCamera` / `UDisplayXRDisplay` base class changed from `UActorComponent` to `USceneComponent`** (via `UDisplayXRRigComponent`). Existing Blueprints load without warnings; the rig node is re-nested under the scene root and can be dragged under the camera it drives. Cooked content needs a re-cook, as for any plugin binary change.

## [0.7.0] - 2026-08-15

### Added
- **Weave-to-texture preview inside the PIE viewport tab** ([#38](https://github.com/DisplayXR/displayxr-unreal/issues/38) M2, Unity-parity, verified weaved 3D on hardware). With `r.DisplayXR.EditorNativePIE 1`, Play shows the runtime's **woven** output in the PIE tab itself — no separate window. The pipeline: UE renders the SBS atlas into the viewport's **own** render target (in editor texture mode the plugin refuses every separate-RT hook — a docked stereo viewport with a separate RT flips Slate's window present onto its stereo-composite path, which renders the whole editor UI into the XR target); `PostRenderViewFamily_RenderThread` copies the atlas into the OpenXR swapchain each frame; the session is bound in texture mode (`sharedTextureHandle` = a worst-case-sized shared D3D12 surface per runtime ADR-010, plus an invisible click-through proxy child window over the viewport as the display processor's interlace-phase anchor); one full-window `XrDisplayZoneDXR` on `xrLocateViews` + the projection layer supplies the weave canvas; and `OnBackBufferReadyToPresent` blits the woven texture 1:1 into the tab via XRBase's format-safe `AddXRCopyTexturePass`. The PIE viewport is also unset from the window's registered-viewport slot for the duration (`RegisterGameViewport` installs it at Play; a registered stereo viewport re-triggers the composite path regardless of the RT refusals). Proxy dims are forced even so the tile halving is exact across UE's view rects, the copy, and the runtime's imageRects. The rect is captured once at PIE start (live move/resize glue is M3). `r.DisplayXR.EditorNativePIEMirror 1` forces the legacy top-level mirror window, which is also the automatic fallback when the runtime lacks `XR_DXR_display_zones`.

- **Live viewport tracking for the in-tab preview** (#38 M3). The preview follows the PIE viewport every editor tick: layout moves (splitters, panel changes) apply immediately — interlace phase is computed from the proxy's live screen rect — while size changes settle through a 0.35 s debounce before the zone, proxy, and blit update together (per-frame canvas resize causes runtime swapchain-realloc storms; whole-window drags are free since the proxy is a child window the display processor tracks on its own). Moving the viewport into a different top-level window (dock↔float) restarts the preview against the new window — a deliberate teardown/rebind rather than `SetParent`, which is the known weaver stereo-collapse trap. Verified live: mid-Play editor resizes settle to even canvases and back with no device hang.

- **The in-tab preview is now the default and only editor preview** — `r.DisplayXR.EditorNativePIE` defaults to `1` (set `0` for no editor preview at all). On a runtime without `XR_DXR_display_zones` the module warns once at Play and the editor stays 2D.

### Removed
- **`FDisplayXRPreviewSession` and the SceneCapture editor preview** (2000+ lines): the standalone second OpenXR session, its raw-Win32 window, and the `USceneCapture2D` render path. The in-tab preview drives the same `FDisplayXRDevice` path a packaged game uses.
- **The raw-Win32 mirror window and `r.DisplayXR.EditorNativePIEMirror`.** The mirror rode the separate-render-target path, which the Slate stereo-composite trap makes unusable in a docked editor; texture mode made it redundant rather than fixable.
- The never-read `FDisplayXRPlatform::bSuppressCompositor`, and the Phase-1 instrumentation logging in `DisplayXRDevice.cpp` (per-call counters, first-call one-shots, `GLog->Flush` spam).

### Fixed
- **Editor play sessions now rebind the OpenXR session each cycle.** The XrSession outlives PIE, so a second Play left it bound to the previous cycle's destroyed window (and never saw the new shared-texture handle — the binding rides `xrCreateSession`). The compositor now recreates the session whenever the editor supplies the bound window; the game path keeps create-once. Also closed the fresh-process race where the first PIE draw could bind a compositor to UE's own window before the proxy existed.
- **An optional extension can no longer take the whole session down.** If `xrCreateInstance` rejects the enabled-extension list, the plugin retries once with only the core set (display info, atlas capture, the graphics/window bindings) and logs loudly. A runtime may advertise an extension and still refuse to enable it — most often an undeclared dependency, which surfaces as `XR_ERROR_VALIDATION_FAILURE` against the list as a whole and names no culprit. Degrading to mono is survivable; creating no instance at all means no HMD device and DisplayXR silently absent.
- **`XR_DXR_display_zones` is vendored, probed, and enabled.** A zone chained on `xrLocateViews` scopes the view-rig framing to a window-pixel rect (the rect *is* the canvas) and the same zone on the projection layer binds it at `xrEndFrame`. This is the canvas source the runtime's weave-to-texture path needs for the in-viewport editor preview ([#38](https://github.com/DisplayXR/displayxr-unreal/issues/38)); nothing submits zones yet, so behavior is unchanged. Availability is exposed as `FDisplayXRSession::HasDisplayZones()` and logged AVAILABLE/ABSENT at instance creation. It is enabled together with its dependencies — `XR_DXR_local_3d_zone` (whose `XrLocal3DZoneMaskDXR` it reuses) and `XR_DXR_view_rig` — or not at all, since OpenXR rejects the entire list when a dependency is missing.

### Fixed
- **Second and later PIE sessions never rebuilt the compositor.** `bCompositorCreationAttempted` was a function-local `static` in `FDisplayXRDevice::UpdateViewport`, so it latched for the whole editor process: pressing Play a second time left the native-PIE path with no compositor and stereo silently off. It is now a device member with an explicit lifecycle — `NotifyPlaySessionEnded()` drops the compositor while the window its session is bound to is still alive (and leaves creation disarmed so teardown can't rebuild against a dying window), and `NotifyPlaySessionStarting()` re-arms it on the next Play. Game/standalone behavior is unchanged: neither hook is called outside the editor.

### Changed
- **ABI pin bumped to `displayxr-runtime@b0e5889` (runtime#803, display-zones spec v3)** to adopt `XR_DXR_display_zones`. The five previously vendored headers changed in comments and `SPEC_VERSION` values only — no struct or ABI change. `SPEC_VERSION` numbers jump (`view_rig` 1→3, `win32_window_binding` 1→8, `display_info` 1→16, `atlas_capture` 1→3, `cocoa` 1→6) because runtime#738 restored the pre-rename `XR_EXT_*` numbering; this is exactly why capability gates test the extension **name** and never the version. `XR_DXR_local_3d_zone.h` is now vendored too — `XR_DXR_display_zones.h` includes it — and both are covered by the `abi-guard` job.
- **The runtime now owns the view math — the plugin computes no Kooima.** Both the runtime device path and the editor preview chain an `XrDisplayRigDXR` / `XrCameraRigDXR` descriptor onto `xrLocateViews` (`XR_DXR_view_rig`) and consume render-ready `XrView{pose, fov}`; the fov is clip-independent, so near/far and UE's reverse-Z convention stay app-side via the new `ProjectionMatrixFromFov`. The runtime also owns the canvas geometry, so the per-frame window-rect resolve is gone. Brings Unreal to parity with [displayxr-unity](https://github.com/DisplayXR/displayxr-unity), which made the same move in `9a2ba6b`. (`DisplayXR/displayxr-runtime` #396 W7, ADR-024; [#36](https://github.com/DisplayXR/displayxr-unreal/issues/36))

### Removed
- **The `displayxr-common` git submodule and the whole `displayxr::math` integration.** Gone with it: the six `*_impl.c` compile shims, both `PrivateIncludePaths` entries, `CStandard = CStandardVersion.C17`, and the scoped C4456 suppression — all of which existed only to compile the vendored library. `.gitmodules` is deleted; `git submodule update --init` is no longer part of setup. Do not re-vendor the math: the `drift-guard` workflow fails on it.

### Notes
- The rig descriptor is submitted with an **identity pose**. `XrView.pose` comes back in the locate space (rig pose + eye displacement) and the rig orientation is baked into the returned fov, while UE applies camera placement and rotation itself in `CalculateStereoViewOffset` — forwarding the camera transform double-counts both. Consequently `SetSceneTransform` no longer feeds the view math and is now diagnostic only.

### Requirements
- Requires a DisplayXR runtime advertising `XR_DXR_view_rig` (>= v2.0.0, already the v0.6.0 minimum — no new requirement). The capability gate tests the extension **name**: released v2.0.0 reports `SPEC_VERSION 1` (numbering restarted at the `XR_EXT_*`→`XR_DXR_*` rename) while carrying the full spec-3 structs, so a version-based gate would wrongly reject it. Without the extension the plugin warns once and renders mono rather than wrong.

## [0.6.0] - 2026-08-01

### Changed
- **BREAKING — DisplayXR OpenXR extensions renamed `XR_EXT_*` → `XR_DXR_*`.** The vendor-specific extensions were sitting in the `EXT` (cross-vendor Khronos) namespace they were never entitled to; they now use the DisplayXR `DXR` tag. Covers `atlas_capture`, `display_info`, `win32_window_binding`, and `cocoa_window_binding` — headers, enum/struct/function names, and extension-name strings. **Requires DisplayXR runtime >= v2.0.0**; older runtimes do not advertise the renamed extensions. (DisplayXR/displayxr-runtime#734)
- Pinned the `displayxr-common` submodule to the released `v2.0.0` ref (was a temporary pre-release SHA).

### Added
- Portable code-signing for the packaged `Binaries/Win64` DLLs via a provider-runner `sign-artifact` workflow (`DXR_SIGN_REPO`), replacing the local-cert-only path. Signing stays capability-gated — an unset env yields an unsigned ZIP rather than a failed build. (#29, #30, #31)

### Fixed
- **Plugin did not compile against the renamed headers.** The rename swept `PFN_xrSetSharedTextureOutputRectEXT` → `...DXR`, but ADR-031 removed that entry point from the runtime API entirely (display zones are the sole region paradigm). The dead, null-guarded output-rect path is gone from the compositor and the editor preview session — behavior-neutral against runtime v2.0.0, which never resolved the pointer.
- **`displayxr-common` v2.0.0 integration.** The OpenXR-typed wrappers are now pure pointer-casts over a new `dxr_view_math.c` core, which no module compiled; both `DisplayXRCore` and `DisplayXREditor` gained a `dxr_view_math_impl.c` shim. The wrappers' C11 `_Static_assert` layout guards also need `CStandard = CStandardVersion.C17` — MSVC's default C mode rejects them.

## [0.5.1] - 2026-07-03

### Changed
- Removed the blanket Ctrl swallow so Ctrl now reaches UE. The runtime + shell arbitrate the Ctrl-prefixed chords (Ctrl+L / Ctrl+1/2/3 / Ctrl+Space), so the plugin no longer swallows Ctrl. Requires DisplayXR runtime v1.27.0+ for the chord arbitration. (#667)
- Relicensed to Apache-2.0; vendored OpenXR extension headers remain BSL-1.0.

### Added
- Packaged `Binaries/Win64/*.dll` are now EV code-signed before zipping. (#666)

## [0.5.0] - 2026-06-29

UE is now a first-class DisplayXR shell citizen — it renders correctly over IPC and behaves correctly under the workspace shell.

### Added
- **Render over IPC (shell / forced-IPC).** The single-tiled `arraySize=1` shared-texture swapchain is non-coherent cross-process from UE's process (proven: even a dedicated device's writes don't reach the D3D11 service). Over IPC, UE now renders both eyes side-by-side into a private RT and `CopyTexture`s each eye into a slice of a canonical `arraySize=2` RGBA swapchain (robust on an engine device). In-process keeps the single-tiled zero-copy path. Resolves the 0.4.3 "content arrives black at the service compositor" known issue. (#23)
- **Camera-look under the shell.** UE routes mouse by capture/cursor-position, not focus, so under the shell (cursor over the shell window, UE never foreground) injected input is dropped. The plugin now feeds the forwarded drag delta straight to the local `PlayerController` (`AddYaw/PitchInput`, the same path the game's own look mapping uses), driving look regardless of focus/capture.
- **Keyboard input under the shell.** Forwarded `WM_KEY*`/`WM_MOUSE*` are relayed from the bound overlay to UE's real window (WASD/QE). Ctrl is intentionally swallowed — the shell reserves it for its chords (Ctrl+L / Ctrl+1/2/3 / Ctrl+Space) and UE's default pawn binds LeftControl to vertical movement.

### Fixed
- **Teardown hang.** Closing the app under the shell hung forever — the runtime drives `EXIT_REQUEST → STOPPING`; `xrEndSession` then queues `EXITING`, but the compositor loop parked on STOPPING before polling again, so EXITING was never received and the app spun a parked session-less loop. The loop now pumps events every iteration (before the park gate), requests engine exit on `EXITING`, and wakes/bails the game thread on park. Closes instantly.
- **Stray fullscreen window over the shell.** UE's own top-level window showed its mono mirror on the desktop. It is now clipped to an empty region (full-size at its origin so the overlay geometry + Kooima and the swapchain present are untouched), and — because UE's blocking startup load stalls the game thread — a small watcher thread (independent of the game thread) hides it the instant it appears, so it no longer covers the shell during load.
- **App didn't show until alt-tab.** UE renders the whole time but grabs OS foreground on launch, so the shell stopped displaying it. The shell's foreground window is captured at module load and handed back after the window is hidden; the app now appears on launch without an alt-tab. Also keeps UE rendering while unfocused (`t.IdleWhenNotForeground=0`, set at module load).

## [0.4.3] - 2026-06-27

### Fixed
- Apps rendered **black** in every mode against runtimes with the CTS session-state contract (`DisplayXR/displayxr-runtime`#33): a graphics session no longer reaches FOCUSED at `xrBeginSession` — `READY→SYNCHRONIZED` fires on the first `xrBeginFrame`, and `SYNCHRONIZED→VISIBLE→FOCUSED` advances only via `xrPollEvent`. v0.4.2 began the session synchronously then stopped polling, so it stuck at SYNCHRONIZED → `xrWaitFrame` reported `shouldRender=false` → the compositor submitted empty frames forever. `CreateSessionWithGraphics` now **warms the session to VISIBLE/FOCUSED** (empty frame + event-drain loop) before the compositor thread starts, restoring the clean first-frame handshake. A new compositor-thread `PumpEvents()` drains lifecycle events (serialized with the frame calls; game-thread polling mid-frame deadlocks the in-process native compositor), and the editor preview gets the same warmup + per-tick drain. Event-drain loops now terminate on `== XR_SUCCESS` rather than `XR_SUCCEEDED` (`xrPollEvent` returns the positive `XR_EVENT_UNAVAILABLE` when the queue is empty, which `XR_SUCCEEDED` treats as success → infinite spin). Verified in-process on Leia hardware. (#21)

### Known issues
- Under the shell / forced-IPC, the session reaches FOCUSED and submits frames but content arrives black at the service compositor — a separate, UE-specific D3D12 IPC-swapchain issue (the native `cube_handle_d3d12_win` app renders fine over the same IPC path). Tracked as a follow-up.

## [0.4.2] - 2026-06-07

### Fixed
- Built apps / standalone games took several seconds for 3D mode and eye tracking to start: the game path created a bare (graphics-less) OpenXR session at module load, then destroyed and recreated it with the D3D12 + window binding at first viewport draw — paying the runtime's expensive session init twice. The session is now created once, with the graphics binding, and begun synchronously (the runtime posts READY at xrCreateSession), matching the editor preview's instant startup. Mac/Linux keep the bare-session path (the D3D12 compositor never runs there).

## [0.4.1] - 2026-06-06

### Added
- CI drift-guard: fails the lint workflow if the shared `displayxr::math` Kooima sources are re-vendored into the plugin instead of consumed via the `displayxr-common` submodule (#396 W5).

### Changed
- Atlas/screenshot capture restored to the runtime-owned `xrCaptureAtlasDXR` path (un-reverts the app-side RHI readback). The runtime bug that produced black/transparent and washed-out PNGs is fixed in `DisplayXR/displayxr-runtime#425` (opaque-alpha encode), so the EXT path from `ext-atlas-capture` is now the default again.
- Capture filenames adopt the runtime-owned suffix `<Project>-<N>_atlas_<viewCount>_<cols>x<rows>.png`: the plugin passes a bare `<Project>-<N>` prefix (no pre-baked layout tokens) and the runtime appends the layout, so the final name no longer duplicates it. Sequence numbering scans the `<Project>-<N>_atlas_*.png` names.
- ABI pin (`.displayxr-runtime-abi`) bumped to runtime `964277f` (the displayxr-runtime#432 merge): vendored `XR_DXR_atlas_capture.h` updated to SPEC_VERSION 2 (struct-identical to v1 — the bump documents the opaque-alpha encode and the layout-encoded filename suffix). Requires runtime ≥ v1.12.0 for correct (non-transparent) captures.

## [0.4.0] - 2026-06-05

### Changed
- Kooima math now comes from the shared [`displayxr-common`](https://github.com/DisplayXR/displayxr-common) `displayxr::math` library (v0.2.0), pinned as a git submodule at `Source/ThirdParty/displayxr-common` and compiled into `DisplayXRCore` and `DisplayXREditor` via `*_impl.c` shims. The vendored `Private/Native/{display3d_view,camera3d_view}.{c,h}` copies are deleted. Run `git submodule update --init` after pulling (the packaged release ZIP already contains the sources).
- `display3d_compute_views` call sites migrated to the superset API: ZDP-anchored `near_offset`/`far_offset` clip (inert here — UE rebuilds its own reverse-Z projection and only consumes `eye_display`) + `vulkan_flip_y=0` (matches the old no-flip behavior).
- Window-relative Kooima input-prep (window-center→display-center offset + screen-Y-down→eye-Y-up flip) in `DisplayXRDevice::ComputeViews` and `DisplayXRPreviewSession` replaced by the library's Layer 1 `display3d_resolve_window_rect()`; only the platform rect fetch stays plugin-side.
- CI: vendor-name guard now excludes verbatim-vendored third-party content (the runtime extension headers enforced byte-for-byte by `abi-guard`, and the `displayxr-common` submodule).

## [0.3.1] - 2026-06-04

### Added
- ABI-drift guard: OpenXR extension headers are now vendored verbatim from `displayxr-runtime` (pinned in `.displayxr-runtime-abi`), with a CI `abi-guard` job that fails on drift.

### Fixed
- Black-window regression on standalone/PIE caused by Win32 window-binding struct ABI drift (the runtime grew `transparentBackgroundEnabled` / `chromaKeyColor`).
- Crash on close (`EXCEPTION_ACCESS_VIOLATION`): the session was destroyed before its child space. Shutdown now destroys ViewSpace → Session → Instance in order.

### Changed
- Atlas/screenshot capture reverted to app-side RHI readback. The runtime `xrCaptureAtlasDXR` path washes out the D3D12/BGRA swapchain (runtime bug DisplayXR/displayxr-runtime#425); the EXT variant lives on branch `ext-atlas-capture`.

## [0.2.1] - 2026-05-07

### Fixed
- `EditorPreview`: real-time Kooima updates during window drag without 3D stutter.

### Changed
- `install-dev` skill: auto-mirror plugin to no-space root for spaced `PLUGIN_SRC`.
- README: added lint + license badges.
- CI: excluded `CHANGELOG.md` from vendor-name guard.
- Added `.github/CODEOWNERS` for sole-reviewer auto-request.

## [0.2.0] - 2026-04-29

### Added
- Atlas capture on 'I' key, exposed via console command and Blueprint API.
- App manifest pipeline for cook/stage (#5).
- Editor preview: native PIE path via `SViewport` stereo flag, gated by CVar; SHIFT+F1 dev shortcut; Phase 3/4 plumbing ported from LeiaUnrealSDK.
- `/install-dev` skill for plugin dev workflow (junctions plugin checkout into the test project, builds `DisplayXRTestEditor` before handoff).
- Window-relative rendering: Kooima, tile layout, and compositor `imageRect` now driven by the host window rather than display-fixed coordinates.
- Live Kooima updates during modal window drag.

### Fixed
- `PackageApp`: UE 5.7+ install path resolution, quoted UAT command, guarded redist step.
- Atlas capture: window-relative atlas dimensions and opaque alpha.
- `WndProc`: bypass UE's aspect-ratio constraint on `WM_SIZING` so the window can be freely resized.

### Changed
- Release skill: split `git push` chain into separate calls; flag benign `PackagePlugin` registry-probe noise.
- `chore`: gitignore Python bytecode (`__pycache__/`).
- `FDisplayXRDevice`: instrumented callbacks for PIE diagnosis.

## [0.1.1] - 2026-04-22

### Fixed
- `DisplayXREditor`: Initialize `LeftEyeRaw` / `RightEyeRaw` to `FVector::ZeroVector` in `FDisplayXRPreviewSession::RenderAndBlit` so the `IsNearlyZero` fallback check no longer reads uninitialized storage when `xrLocateViews` returns `ViewCount < 2`. Silences MSVC C4701.

## [0.1.0] - 2026-04-21

Initial tagged release of the DisplayXR Unreal plugin.

### Added
- Three plugin modules:
  - `DisplayXRCore` (Runtime, Win64/Mac/Android) — OpenXR pipeline integration, stereo rigs, Kooima asymmetric frustum projection driven by real-time eye positions from the DisplayXR runtime.
  - `DisplayXRMaterials` (Runtime, all platforms) — custom material expressions for stereoscopic rendering.
  - `DisplayXREditor` (Editor, Win64/Mac) — editor-side visualization and preview tooling.
- Two stereo rig modes: camera-centric (attach to an existing pawn camera) and display-centric (virtual display in the scene).
- Zero-copy atlas handoff — UE renders directly into the OpenXR swapchain.
- Engine pin: `EngineVersion` now set to `5.7.0` in `DisplayXR.uplugin`.
- Packaged-plugin ZIP distribution via GitHub Releases — consumers pin a version in `.displayxr-version` and fetch with the helper script in `displayxr-unreal-test`.
