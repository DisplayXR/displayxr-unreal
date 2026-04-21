# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## Project Overview

DisplayXR is an Unreal Engine plugin for rendering on eye-tracked 3D light field displays via the DisplayXR OpenXR runtime. The runtime handles vendor integration (interlacing, eye tracking, display processing); this plugin hooks Unreal's rendering into the runtime's OpenXR session.

Current plugin version: `DisplayXR.uplugin` → `"VersionName"`.

## Build System

No CMake — the plugin builds directly through Unreal's build system (UBT). Drop the repo into a UE project's `Plugins/DisplayXR/` folder and regenerate project files.

**Prerequisites:** UE 5.3+, Visual Studio (Windows) or Xcode (macOS), DisplayXR OpenXR runtime installed.

**Packaging for distribution** (Windows, from `Scripts/`):
```
.\PackagePlugin.bat <UE_VERSION>      # e.g. 5.6
```
Output goes to `Packages/DisplayXR_<version>/`.

## Architecture

### Three Modules

1. **DisplayXRCore** (Runtime, `Win64|Mac|Android`, `PostConfigInit`) — OpenXR integration, stereo device, camera components, Kooima C libraries, Blueprint function library.
2. **DisplayXRMaterials** (Runtime, all platforms, `Default`) — Custom material expression nodes (`StereoIndex`, `StereoSelect`, `SideBySideCoords`, `TopBottomCoords`).
3. **DisplayXREditor** (Editor, `Win64|Mac`, `PostEngineInit`) — Editor preview session, viewport widget, component proxies.

### OpenXR Integration

One unified session — `FDisplayXRSession` — loads the DisplayXR runtime **directly** on every platform (`LoadLibraryW` + `XrNegotiateLoaderRuntimeInterface` on Windows, `dlopen` on Mac/Linux). UE's `OpenXR` plugin is **not** a dependency; `DisplayXR.uplugin` declares only `XRBase`. `FDisplayXRCoreModule` implements `IHeadMountedDisplayModule` and bumps its HMD plugin priority +10 above `OpenXRHMD` / `SteamVR` so UE's HMD discovery picks us.

There is **no** `DISPLAYXR_USE_UNREAL_OPENXR` compile flag. Platform differences (DLL loading, window binding, D3D12 vs Metal graphics binding) live inside session and compositor code behind `#if PLATFORM_WINDOWS / PLATFORM_MAC / PLATFORM_LINUX`. See [`Docs/DisplayXR/Architecture.md`](./Docs/DisplayXR/Architecture.md) for the full picture and [`Docs/DisplayXR/adr/ADR-001-direct-runtime-loading.md`](./Docs/DisplayXR/adr/ADR-001-direct-runtime-loading.md) for why.

### Rendering Pipeline

`FDisplayXRDevice` extends `FHeadMountedDisplayBase` + `FXRRenderTargetManager` + `FSceneViewExtensionBase`. It:

1. Polls the OpenXR session each frame to get eye positions.
2. Feeds raw eyes through the Kooima C libraries (`camera3d_view.c`, `display3d_view.c`) for asymmetric frustum projection.
3. Builds UE-native reverse-Z off-axis projection matrices via `DisplayXRStereoMath.h::CalculateOffAxisProjectionMatrix`.
4. UE renders directly into the OpenXR swapchain (zero-copy atlas handoff — see `Docs/DisplayXR/AtlasHandoff.md`).

### Component Hierarchy

- `UDisplayXRCamera` — Camera-centric rig. Properties: `Enable3D`, `ConvergenceDistance`, `LookaroundFactor`, `BaselineFactor`.
- `UDisplayXRDisplay` — Display-centric rig. Stereo computed relative to the display's world transform.
- `ADisplayXRRigManager` — Selects the active rig at play time.

### Public API

- `DisplayXRCamera.h` / `DisplayXRDisplay.h` — Component classes
- `DisplayXRFunctionLibrary.h` — Blueprint-exposed utility functions
- `DisplayXRTypes.h` — Shared type definitions

## Shared native code

`Source/DisplayXRCore/Private/Native/camera3d_view.{c,h}` and `display3d_view.{c,h}` are portable C libraries shared with the Unity sibling plugin (`displayxr-unity`). Keep them engine-agnostic — no UE types, no Unity types.

## CI/CD

- `.github/workflows/lint.yml` — PR-to-main lint checks: vendor-name guard, valid JSON in `DisplayXR.uplugin`, valid YAML in workflows. Runs on `ubuntu-latest`.
- Full UE build is not in CI (UE is not on github-hosted runners). Tracked as future work in `Docs/DisplayXR/TODO.md`.

## Documentation

- `README.md` — project overview, setup, TOC
- `Docs/DisplayXR/` — design docs (`AtlasHandoff`, `DisplayRigSetup`, `EditorPreview`, `EyeTracking`, `MacSetup`, `CompositorIntegration`)
- `Docs/DisplayXR/TODO.md` — outstanding work, parity items with `displayxr-unity`, in-flight branches

## Sibling repositories

- **[DisplayXR/displayxr-unity](https://github.com/DisplayXR/displayxr-unity)** — Unity plugin. Reference implementation; shares native C Kooima code.
- **[dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)** — DisplayXR OpenXR runtime.
