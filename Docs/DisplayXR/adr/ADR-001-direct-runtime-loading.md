# ADR-001 — Direct OpenXR runtime loading

**Status:** Accepted
**Date:** 2026-04

## Context

Early drafts of this plugin envisioned a dual-path integration: on Windows/Android we'd register as an `IOpenXRExtensionPlugin` extending UE's `FOpenXRHMD`; on macOS we'd bypass UE's OpenXR entirely and drive a direct session because UE does not ship its OpenXR plugin on Mac.

That split is still visible in stale docs. The actual code consolidated on one approach.

## Decision

`FDisplayXRSession` loads the DisplayXR OpenXR runtime directly on every platform:

- Windows: `LoadLibraryW` on the runtime DLL and use `XrNegotiateLoaderRuntimeInterface` to bind. This gives an **in-process** runtime (the runtime's compositor lives in the UE process) rather than going through `openxr_loader.dll` → `DisplayXRClient.dll` IPC.
- macOS / Linux: `dlopen` on the runtime `.so` / `.dylib`.

UE's OpenXR plugin is **not** a dependency. `DisplayXR.uplugin` declares only `XRBase`. `FDisplayXRCoreModule` implements `IHeadMountedDisplayModule` and bumps its own HMD plugin priority +10 above `OpenXRHMD` / `SteamVR` so UE's HMD discovery picks DisplayXR first.

Platform differences (DLL loading, window binding, graphics binding) live inside session and compositor code behind `#if PLATFORM_WINDOWS / PLATFORM_MAC / PLATFORM_LINUX`. There is no product-level compile flag.

## Alternatives considered

1. **`IOpenXRExtensionPlugin` on top of UE's `FOpenXRHMD`.** Rejected: it would force us to inherit UE's session lifecycle, its swapchain choices, and its frame pacing. The custom compositor path needs control over `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` on a dedicated thread. Extension-plugin hooks are too narrow.
2. **Dual implementation split by compile flag (`DISPLAYXR_USE_UNREAL_OPENXR`).** Attempted in early planning, never materialized in code. Would have meant carrying two entire integrations for no real win once the runtime became cross-platform.
3. **Load via `openxr_loader.dll` (the Khronos loader).** Rejected on Windows because it routes to an IPC client (`DisplayXRClient.dll`). We want the in-process compositor so the runtime can weave directly against our D3D12 device without cross-process texture sharing.

## Consequences

- One unified session class. Easier to reason about and to change.
- UE's `FOpenXRHMD` never enters the picture. Any agent thinking "extend UE's OpenXR plugin" is on the wrong track.
- Runtime installation is our responsibility — we don't piggyback on whatever runtime UE's OpenXR chose.
- Platform-conditional code lives inside session/compositor, not spread across two class trees.
- HMD plugin priority must stay higher than `OpenXRHMD`/`SteamVR`. If someone enables those alongside DisplayXR, the +10 bump in `FDisplayXRCoreModule::StartupModule()` keeps us active.

## Related

- [Architecture.md](../Architecture.md) — overall structure
- [AtlasHandoff.md](../AtlasHandoff.md) — consequence: UE renders into our swapchain, not UE's
