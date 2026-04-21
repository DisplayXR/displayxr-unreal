# DisplayXR — Unreal Plugin

Unreal Engine plugin for rendering on eye-tracked 3D light field displays via the DisplayXR OpenXR runtime. Works with any OpenXR-compatible 3D display.

## Table of Contents

- [Overview](#overview)
- [Requirements](#requirements)
- [Installing the Plugin](#installing-the-plugin)
- [Scene Setup](#scene-setup)
  - [Camera-Centric Mode](#camera-centric-mode)
  - [Display-Centric Mode](#display-centric-mode)
- [Building Your App](#building-your-app)
- [Documentation](#documentation)
- [Related Repositories](#related-repositories)

> **New to the plugin?** Start with the [Display Rig Setup guide](Docs/DisplayXR/DisplayRigSetup.md) — it covers the pawn and camera configuration needed for stereo 3D, plus the differences between the two rig modes. For outstanding work and planned parity with the Unity sibling plugin, see [TODO](Docs/DisplayXR/TODO.md).

---

## Overview

The plugin hooks into Unreal's OpenXR pipeline to provide:

- **Eye-tracked stereo rendering** — Kooima asymmetric frustum projection driven by real-time eye positions from the DisplayXR runtime
- **Two stereo rig modes** — camera-centric (add to an existing pawn camera) or display-centric (place a virtual display in the scene)
- **Zero-copy atlas handoff** — UE renders directly into the OpenXR swapchain (see [AtlasHandoff](Docs/DisplayXR/AtlasHandoff.md))
- **Editor preview** — a standalone OpenXR session in the editor so you can see stereo output without running PIE (see [EditorPreview](Docs/DisplayXR/EditorPreview.md))

One unified session loads the DisplayXR OpenXR runtime directly on every platform; UE's `OpenXR` plugin is **not** a dependency. Platform differences (Windows D3D12, macOS Metal, Android Vulkan) live inside the session and compositor, not in a product-level compile flag. See [Architecture.md](Docs/DisplayXR/Architecture.md) for the full picture.

## Requirements

- Unreal Engine 5.3 or later
- DisplayXR OpenXR runtime installed (see [openxr-3d-display](https://github.com/dfattal/openxr-3d-display))
- Windows, macOS, or Android target
- Visual Studio (Windows) or Xcode (macOS) matching your UE version

## Installing the Plugin

1. Create a UE project (or open an existing one).
2. Make a `Plugins` folder next to the `.uproject` if one doesn't exist.
3. Clone this repository to `Plugins/DisplayXR`.
4. Regenerate project files and build the project from your IDE.

## Scene Setup

### Camera-Centric Mode

Attach a `UDisplayXRCamera` component to your pawn's camera. Set `ConvergenceDistance`, `LookaroundFactor`, and `BaselineFactor` as desired. The rig rotates with the camera.

### Display-Centric Mode

Attach a `UDisplayXRDisplay` component to an actor representing a virtual display in the scene. Stereo is computed relative to the display's world transform.

See [DisplayRigSetup](Docs/DisplayXR/DisplayRigSetup.md) for the full walkthrough.

## Building Your App

Use the provided script to package the plugin against a target UE version:

```
Scripts\PackagePlugin.bat 5.6
```

Output lands in `Packages/DisplayXR_<version>/`. Drop the packaged plugin into your shipping project's `Plugins/` folder.

For per-project builds, use UAT directly (see `Scripts/PackageApp.py` for a reference invocation).

## Documentation

In-depth docs live in [Docs/DisplayXR/](Docs/DisplayXR/):

- [QuickStart](Docs/DisplayXR/QuickStart.md) — install runtime → install plugin → press Play
- [Architecture](Docs/DisplayXR/Architecture.md) — one-page class hierarchy, ownership, per-frame flow
- [AtlasHandoff](Docs/DisplayXR/AtlasHandoff.md) — zero-copy UE → OpenXR swapchain pipeline
- [DisplayRigSetup](Docs/DisplayXR/DisplayRigSetup.md) — pawn/camera rig configuration, input, rig modes
- [EditorPreview](Docs/DisplayXR/EditorPreview.md) — current `SceneCapture2D`-based preview
- [EditorPreviewNative](Docs/DisplayXR/EditorPreviewNative.md) — in-flight plan for native `FDisplayXRDevice` → PIE preview
- [EyeTracking](Docs/DisplayXR/EyeTracking.md) — `xrLocateViews` → Kooima → per-view projection pipeline
- [MacSetup](Docs/DisplayXR/MacSetup.md) — macOS-specific setup quirks
- [CompositorIntegration](Docs/DisplayXR/CompositorIntegration.md) — prior compositor design (historical)
- [ADRs](Docs/DisplayXR/adr/) — decision records for load-bearing architectural choices
- [TODO](Docs/DisplayXR/TODO.md) — outstanding work, parity items, in-flight branches

## Related Repositories

- **[dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)** — DisplayXR OpenXR runtime. Extensions: `XR_EXT_display_info`, `XR_EXT_win32_window_binding`, `XR_EXT_cocoa_window_binding`.
- **[DisplayXR/displayxr-unity](https://github.com/DisplayXR/displayxr-unity)** — Unity sibling plugin. Reference implementation for rig patterns, editor preview, and shared Kooima C libraries.
