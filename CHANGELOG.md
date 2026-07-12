# Changelog

All notable changes to the DisplayXR Unreal plugin are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

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
