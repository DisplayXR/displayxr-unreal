# Changelog

All notable changes to the DisplayXR Unreal plugin are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

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
