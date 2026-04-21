# DisplayXR Unreal Plugin — Documentation

Unreal Engine integration with the DisplayXR OpenXR runtime for eye-tracked 3D light field displays.

> **New here?** Start with the [Quick Start](./QuickStart.md) guide. Then read [Architecture.md](./Architecture.md) for the mental model.

## Quick Links

### Getting started
- [Quick Start](./QuickStart.md) — Install runtime → install plugin → open a project → press Play
- [Display Rig Setup](./DisplayRigSetup.md) — How to set up the pawn/camera rig for stereo 3D (camera rotation, input, display-centric vs camera-centric)
- [Mac Setup](./MacSetup.md) — macOS-specific quirks (OpenXR active-runtime switching, `.uplugin` dependencies)

### Architecture
- [Architecture](./Architecture.md) — One-page class hierarchy, ownership, per-frame data flow
- [Atlas Handoff](./AtlasHandoff.md) — Zero-copy UE → OpenXR swapchain (single-device D3D12, Acquire/Release handshake)
- [Eye Tracking](./EyeTracking.md) — Parallax pipeline: `xrLocateViews` → Kooima → per-view projection, coordinate conventions
- [ADRs](./adr/) — Decision records for load-bearing choices (direct runtime loading, zero-copy atlas, UE-native projection)

### Editor preview
- [Editor Preview](./EditorPreview.md) — Current `SceneCapture2D`-based preview strategy, Unity vs Unreal differences
- [Editor Preview: Native XR Path](./EditorPreviewNative.md) — In-flight plan to replace the current preview with a native `FDisplayXRDevice` → PIE hookup (see also the [agent prompt](./EditorPreviewNative-AgentPrompt.md))

### Historical / planning
- [Compositor Integration](./CompositorIntegration.md) — Prior "separate device + IPC" compositor design (superseded by the single-device path; kept for context)
- [TODO](./TODO.md) — Outstanding work, parity with the Unity sibling, in-flight branches

## Reference Repositories

- **[dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)** — The DisplayXR OpenXR runtime. Defines `XR_EXT_display_info`, window binding extensions, display processor interface.
- **[DisplayXR/displayxr-unity](https://github.com/DisplayXR/displayxr-unity)** — Unity sibling plugin. Shares the Kooima C libraries and rig patterns.
