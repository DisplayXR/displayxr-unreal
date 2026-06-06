# Changelog

All notable changes to the DisplayXR Unreal plugin are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

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
- Atlas/screenshot capture reverted to app-side RHI readback. The runtime `xrCaptureAtlasEXT` path washes out the D3D12/BGRA swapchain (runtime bug DisplayXR/displayxr-runtime#425); the EXT variant lives on branch `ext-atlas-capture`.

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
