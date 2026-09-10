# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## Project Overview

DisplayXR is an Unreal Engine plugin for rendering on eye-tracked 3D light field displays via the DisplayXR OpenXR runtime. The runtime handles vendor integration (interlacing, eye tracking, display processing); this plugin hooks Unreal's rendering into the runtime's OpenXR session.

Current plugin version: `DisplayXR.uplugin` → `"VersionName"`.

## Build System

No CMake — the plugin builds directly through Unreal's build system (UBT). Drop the repo into a UE project's `Plugins/DisplayXR/` folder and regenerate project files.

**Prerequisites:** UE 5.7, Visual Studio (Windows) or Xcode (macOS), DisplayXR OpenXR runtime installed.

**Packaging for distribution** (Windows, from `Scripts/`):
```
.\PackagePlugin.bat <UE_VERSION>      # e.g. 5.6
```
Output goes to `Packages/DisplayXR_<version>/`.

## Architecture

### Three Modules

1. **DisplayXRCore** (Runtime, `Win64|Mac|Android`, `PostConfigInit`) — OpenXR integration, stereo device, camera components, Kooima C libraries, Blueprint function library.
2. **DisplayXRMaterials** (Runtime, all platforms, `Default`) — Custom material expression nodes (`StereoIndex`, `StereoSelect`, `SideBySideCoords`, `TopBottomCoords`).
3. **DisplayXREditor** (Editor, `Win64|Mac`, `PostEngineInit`) — In-tab weaved PIE preview (`FDisplayXRPIEPreview`, Windows only), component proxies.

### OpenXR Integration

One unified session — `FDisplayXRSession` — loads the DisplayXR runtime **directly** on every platform (`LoadLibraryW` + `XrNegotiateLoaderRuntimeInterface` on Windows, `dlopen` on Mac/Linux). UE's `OpenXR` plugin is **not** a dependency; `DisplayXR.uplugin` declares only `XRBase`. `FDisplayXRCoreModule` implements `IHeadMountedDisplayModule` and bumps its HMD plugin priority +10 above `OpenXRHMD` / `SteamVR` so UE's HMD discovery picks us.

There is **no** `DISPLAYXR_USE_UNREAL_OPENXR` compile flag. Platform differences (DLL loading, window binding, D3D12 vs Metal graphics binding) live inside session and compositor code behind `#if PLATFORM_WINDOWS / PLATFORM_MAC / PLATFORM_LINUX`. See [`Docs/DisplayXR/Architecture.md`](./Docs/DisplayXR/Architecture.md) for the full picture and [`Docs/DisplayXR/adr/ADR-001-direct-runtime-loading.md`](./Docs/DisplayXR/adr/ADR-001-direct-runtime-loading.md) for why.

### Rendering Pipeline

`FDisplayXRDevice` extends `FHeadMountedDisplayBase` + `FXRRenderTargetManager` + `FSceneViewExtensionBase`. It:

1. Polls the OpenXR session each frame to get eye positions.
2. Chains an `XR_DXR_view_rig` descriptor onto `xrLocateViews`, so the **runtime** applies the view math and returns render-ready `XrView{pose, fov}` (see §Shared native code).
3. Builds UE-native reverse-Z off-axis projection matrices via `DisplayXRStereoMath.h::CalculateOffAxisProjectionMatrix`.
4. UE renders directly into the OpenXR swapchain (zero-copy atlas handoff — see `Docs/DisplayXR/AtlasHandoff.md`).

### Component Hierarchy

- `UDisplayXRRigComponent` — Abstract `USceneComponent` base for both rigs. A rig attaches under the camera it drives (else binds to the owner's first camera) and fills `BuildTunables()`; rigs do not tick or push.
- `UDisplayXRCamera` — Camera-centric rig. Properties: `IpdFactor`, `ParallaxFactor`, `InvConvergenceDistance`. The camera's horizontal `FieldOfView` is converted to the rig's vertical fov with UE's own projection rules.
- `UDisplayXRDisplay` — Display-centric rig. The camera transform IS the display plane; the viewer moves around it. Properties: `IpdFactor`, `ParallaxFactor`, `PerspectiveFactor`, `VirtualDisplayHeight` (metres).
- `FDisplayXRRigManager` — Static registry and the single tunables pusher: once per frame (`SetupViewFamily`) it pushes the rig on the camera the local player renders from, or defaults when that camera has no rig.
- Editor: `FDisplayXRCameraVisualizer` / `FDisplayXRDisplayVisualizer` draw the convergence plane / display plane while the component is selected.

### Public API

- `DisplayXRCamera.h` / `DisplayXRDisplay.h` — Component classes
- `DisplayXRFunctionLibrary.h` — Blueprint-exposed utility functions
- `DisplayXRTypes.h` — Shared type definitions

## Shared native code

**There is none, and that is deliberate.** The plugin computes no view math of
its own: the DisplayXR runtime owns it via the `XR_DXR_view_rig` extension
(`DisplayXR/displayxr-runtime` #396 W7, ADR-024). Both the runtime device path
and the editor preview chain an `XrDisplayRigDXR` / `XrCameraRigDXR` descriptor
onto `xrLocateViews` and consume render-ready `XrView{pose, fov}`; the fov is
clip-independent, so near/far and UE's reverse-Z convention stay app-side
(`DisplayXRStereoMath.h::ProjectionMatrixFromFov`).

Consequences worth knowing:

- **No `displayxr-common` submodule, no `displayxr::math` link, no FetchContent.**
  Do NOT re-vendor `display3d_view.*` / `camera3d_view.*` — the `drift-guard`
  workflow fails the build if those files reappear as tracked sources. This
  matches [`displayxr-unity`](https://github.com/DisplayXR/displayxr-unity),
  which dropped the same dependency.
- **Requires a runtime advertising `XR_DXR_view_rig`** (DisplayXR runtime
  >= v2.0.0). Without it the plugin has nothing to fall back to: it emits a
  one-shot WARN and renders mono rather than wrong.
- **Gate on the extension NAME, never on `SPEC_VERSION`.** Released runtime
  v2.0.0 advertises `XR_DXR_view_rig_SPEC_VERSION 1` (numbering restarted at the
  `XR_EXT_*` -> `XR_DXR_*` rename) while carrying the full spec-3 struct set;
  `displayxr-runtime@main` renumbered it to 3 to continue the pre-rename
  sequence. A `>= 2` check would reject a perfectly capable runtime.
- **Probe before requesting.** `xrCreateInstance` fails outright on an
  unsupported extension, so both sessions call
  `xrEnumerateInstanceExtensionProperties` first and only then add the name to
  the enabled list.
- The extension header is vendored verbatim at
  `Source/DisplayXRCore/Private/Native/openxr/XR_DXR_view_rig.h` and policed by
  the `abi-guard` job against `.displayxr-runtime-abi`, exactly like the other
  four `XR_DXR_*` headers.

## CI/CD

- `.github/workflows/lint.yml` — PR-to-main lint checks: vendor-name guard, valid JSON in `DisplayXR.uplugin`, valid YAML in workflows. Runs on `ubuntu-latest`.
- Full UE build is not in CI (UE is not on github-hosted runners). Tracked as future work in `Docs/DisplayXR/TODO.md`.

## Releasing

Preferred path: `/release` skill at `.claude/skills/release/SKILL.md`.
Tags, packages the plugin via `Scripts/PackagePlugin.bat`, creates a
GitHub Release with the packaged zip attached.

Manual fallback:
```bash
git tag -a vX.Y.Z -m "release notes ..."
git push origin vX.Y.Z
```

### Independent of the runtime's `versions.json` auto-bump matrix

The DisplayXR runtime maintains a `versions.json` at its root that
pins the **bundled stack** (runtime, shell, leia-plugin, mcp, demos)
for the dev orchestrator and the meta-installer. **This Unreal plugin
is intentionally NOT in that matrix** — it's a downstream consumer
of the runtime's OpenXR wire protocol, not part of the co-released
bundle. The Unreal sibling [`displayxr-unity`](https://github.com/DisplayXR/displayxr-unity)
shares the same boundary. See
[`displayxr-runtime/docs/specs/runtime/versions-json-autobump.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md)
for the auto-bump spec; this plugin doesn't participate.

## Documentation

- `README.md` — project overview, setup, TOC
- `Docs/DisplayXR/` — design docs (`AtlasHandoff`, `DisplayRigSetup`, `EditorPreview`, `EyeTracking`, `MacSetup`, `CompositorIntegration`)
- `Docs/DisplayXR/TODO.md` — outstanding work, parity items with `displayxr-unity`, in-flight branches

## Sibling repositories

- **[DisplayXR/displayxr-unity](https://github.com/DisplayXR/displayxr-unity)** — Unity plugin. Reference implementation; shares native C Kooima code.
- **[DisplayXR/displayxr-runtime](https://github.com/DisplayXR/displayxr-runtime)** — DisplayXR OpenXR runtime. (Old URL `dfattal/openxr-3d-display` redirects, but reference the canonical org URL in new docs.)
- **[DisplayXR/displayxr-mcp](https://github.com/DisplayXR/displayxr-mcp)** — MCP framework. Not consumed by this plugin today; future possibility if Unreal-side agent surface becomes a thing.
- **[DisplayXR/displayxr-leia-plugin](https://github.com/DisplayXR/displayxr-leia-plugin)** — Leia SR display-processor plug-in for the runtime. Vendor integration is runtime-side; this plugin doesn't talk to the SR SDK directly.
- **[DisplayXR/displayxr-installer](https://github.com/DisplayXR/displayxr-installer)** — Meta-installer bundle for end users. Bundles runtime + shell + leia + mcp + demos. This Unreal plugin is NOT bundled (see §Releasing).
