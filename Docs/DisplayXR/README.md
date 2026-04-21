# DisplayXR Unreal Plugin — Documentation

Unreal Engine integration with the DisplayXR OpenXR runtime for eye-tracked 3D light field displays.

## Quick Links

- [Atlas Handoff](./AtlasHandoff.md) — How UE renders directly into the OpenXR swapchain (zero-copy): single-device D3D12, `AllocateRenderTargetTextures`+`AcquireColorTexture`, per-frame Acquire/Release handshake
- [Compositor Integration](./CompositorIntegration.md) — Original Phase 2 planning doc (historical)
- [Display Rig Setup](./DisplayRigSetup.md) — How to set up the pawn/camera rig for stereo 3D (camera rotation, input, display-centric vs camera-centric)
- [Eye Tracking](./EyeTracking.md) — Parallax pipeline: `xrLocateViews` → Kooima → per-view projection, coordinate conventions
- [Editor Preview](./EditorPreview.md) — Preview strategy, Unity vs Unreal differences
- [Mac Setup](./MacSetup.md) — macOS-specific setup, direct OpenXR session, scene view extension
- [TODO](./TODO.md) — Outstanding work, parity with the Unity sibling, in-flight branches

## Platform Architecture

The plugin has **two code paths** selected at compile time:

| | Windows / Android | macOS |
|---|---|---|
| **How** | `IOpenXRExtensionPlugin` hooks into UE's `FOpenXRHMD` | `FDisplayXRDirectSession` loads runtime via `dlopen` |
| **Stereo injection** | `InsertOpenXRAPILayer` hooks `xrLocateViews` | `FSceneViewExtensionBase` overrides camera projection |
| **Compile flag** | `DISPLAYXR_USE_UNREAL_OPENXR=1` | `DISPLAYXR_USE_UNREAL_OPENXR=0` |
| **Why different** | UE ships OpenXR plugin on Windows/Android | UE does **not** ship OpenXR plugin on macOS |

Everything else is shared: components, rig manager, Kooima C libraries, Blueprint API, materials module. The `FDisplayXRPlatform` abstraction routes calls to the active backend.

## Reference Repositories

- **[dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)** — The DisplayXR OpenXR runtime. Defines `XR_EXT_display_info`, window binding extensions, display processor interface.
- **[DisplayXR/displayxr-unity](https://github.com/DisplayXR/displayxr-unity)** — Unity sibling plugin. Shares the Kooima C libraries and rig patterns.
