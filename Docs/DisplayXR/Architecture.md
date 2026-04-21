# Architecture

One-page reference for the DisplayXR Unreal plugin. Covers the class hierarchy, who owns what, and the per-frame data flow. Read this before touching session, device, or compositor code.

## TL;DR

- **One module**, `FDisplayXRCoreModule`, implements `IHeadMountedDisplayModule` and makes itself the active HMD with priority +10 above `OpenXRHMD` / `SteamVR`.
- **One session**, `FDisplayXRSession`, loads the DisplayXR OpenXR runtime *directly* (not through UE's `OpenXRHMD`) and owns the `XrInstance` / `XrSession`.
- **One custom HMD device**, `FDisplayXRDevice`, extending `FHeadMountedDisplayBase + FXRRenderTargetManager + FSceneViewExtensionBase`. UE drives it through the standard HMD interfaces.
- **One compositor thread**, `FDisplayXRCompositor`, handles the `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` handshake off UE's game and render threads. UE renders directly into the OpenXR swapchain (zero-copy, see [AtlasHandoff.md](./AtlasHandoff.md)).
- **No dual compile-time paths.** Platform differences (DLL loading, window binding, D3D12 vs Metal) live inside session and compositor code behind `#if PLATFORM_WINDOWS / PLATFORM_MAC / PLATFORM_LINUX`. There is no `DISPLAYXR_USE_UNREAL_OPENXR` or similar product-level flag — if you see it referenced somewhere, the doc is stale.

## Module map

| Module | Purpose | Load phase |
|---|---|---|
| `DisplayXRCore` | Session, device, compositor, rig components, Blueprint API, Kooima C libs | `PostConfigInit` |
| `DisplayXRMaterials` | Material expression nodes (`StereoIndex`, `StereoSelect`, `SideBySideCoords`, `TopBottomCoords`) | `Default` |
| `DisplayXREditor` | Editor preview session, component visualization proxies, rig editor UI | `PostEngineInit` (editor only) |

Only the `XRBase` UE plugin is required (`DisplayXR.uplugin`). The plugin **does not** depend on UE's `OpenXR` plugin.

## Class hierarchy

```
FDisplayXRCoreModule          (IHeadMountedDisplayModule)
  ├── Owns FDisplayXRSession  (unified OpenXR runtime wrapper)
  │     ├── XrInstance, XrSession, XrSpace handles
  │     ├── Per-frame Tick(): xrPollEvent, xrLocateViews, stores eyes
  │     ├── Double-buffered FDisplayXRTunables + FDisplayXRViewConfig
  │     └── Owns FDisplayXRCompositor (on Windows/D3D12 path)
  │           ├── Dedicated FRunnableThread (xrWaitFrame/Begin/End loop)
  │           ├── OpenXR swapchain wrapped as TArray<FTextureRHIRef>
  │           └── Per-frame BeginFrameReady ↔ EndFrameReady events
  │
  └── CreateTrackingSystem() → FDisplayXRDevice   (UE's view of the HMD)
         : FHeadMountedDisplayBase     — IXRTrackingSystem / IHeadMountedDisplay
         , FXRRenderTargetManager      — AllocateRenderTargetTexture, etc.
         , FSceneViewExtensionBase     — SetupView, SetupViewPoint, PreRender, ...

FDisplayXRPlatform (static)   — Routes API calls to the active session via
                                FDisplayXRCoreModule::GetSession(). Components
                                and Blueprint functions call this, never the
                                session directly.

FDisplayXRRigManager (static) — Registry of UCameraComponent ↔ UDisplayXRCamera
                                pairs. Picks the active rig so only one pushes
                                tunables per frame.

UDisplayXRCamera (UActorComponent) — Camera-centric rig tunables
UDisplayXRDisplay (UActorComponent) — Display-centric rig tunables

[Editor module]
FDisplayXRPreviewSession    — Current SceneCapture2D-based editor preview
                              (see EditorPreview.md; replacement tracked in
                              EditorPreviewNative.md).
UDisplayXRCameraProxy /     — UPrimitiveComponent proxies for editor
UDisplayXRDisplayProxy        visualization of the rig transform/frustum.
```

## Per-frame data flow (game mode, Windows/D3D12)

```
Compositor thread (independent loop):
  1. xrWaitFrame                               (blocks on runtime vsync)
  2. xrBeginFrame
  3. Signal BeginFrameReadyEvent
  4. Wait EndFrameReadyEvent (≤ 50ms timeout)
  5. xrEndFrame with XrCompositionLayerProjection

Game thread:
  FDisplayXRSession::Tick()
    → xrPollEvent (session state transitions)
    → xrLocateViews (predictedDisplayTime)
    → Write FDisplayXRViewConfig + eye positions to buffer[1-readIdx]
    → Atomic swap read index

  FDisplayXRDevice::UpdateViewport()
    → FDisplayXRCompositor::AcquireImage_GameThread()
        → Wait BeginFrameReadyEvent
        → xrAcquireSwapchainImage + xrWaitSwapchainImage
        → Return image index

  UE scene setup runs. FSceneViewExtensionBase callbacks on FDisplayXRDevice
  feed per-view Kooima projection into FSceneViewFamily.

Render thread:
  UE renders the scene directly into the acquired swapchain FRHITexture.
  FDisplayXRCompositor::ReleaseImage_RenderThread()
    → RHI transition → Present
    → xrReleaseSwapchainImage
    → Signal EndFrameReadyEvent     (unblocks step 4 of compositor loop)
```

The zero-copy detail — how the OpenXR swapchain image becomes an `FRHITexture` UE will render into — is in [AtlasHandoff.md](./AtlasHandoff.md).

## Component interface (game code)

Game code and Blueprint nodes never call `FDisplayXRSession` directly. They go through `FDisplayXRPlatform` (static helpers in `DisplayXRPlatform.h`):

```cpp
FDisplayXRPlatform::SetTunables(tunables);            // IPD / parallax / near-far
FDisplayXRPlatform::SetSceneTransform(xform, bEnabled);
FDisplayXRPlatform::GetDisplayInfo();                  // physical display dimensions
FDisplayXRPlatform::GetEyePositions(L, R, bTracked);
FDisplayXRPlatform::RequestDisplayMode(bMode3D);
FDisplayXRPlatform::RequestEyeTrackingMode(bManual);
```

`FDisplayXRPlatform` routes to the active session. If no session is live (e.g. runtime not installed), calls no-op and return defaults — the game still runs in 2D.

## Kooima math lives in portable C

`Source/DisplayXRCore/Private/Native/{camera3d_view, display3d_view}.{c,h}` are portable C, shared byte-for-byte with the [displayxr-unity](https://github.com/DisplayXR/displayxr-unity) sibling plugin. Keep them engine-agnostic — no UE or Unity types. Sync changes both directions.

`DisplayXRStereoMath.h` wraps the Kooima output into UE-native reverse-Z off-axis projection matrices (see [ADR-003](./adr/ADR-003-ue-native-off-axis-projection.md) for why we don't consume Kooima's `projection_matrix[16]` directly).

## Platform notes

- **Windows / D3D12** is the primary development path. Session loads the runtime DLL via `LoadLibraryW` + the OpenXR `XrNegotiateLoaderRuntimeInterface`, which gives us an **in-process compositor** (not `openxr_loader.dll` + IPC). DPI awareness is set to per-monitor at module startup so the backbuffer uses physical pixels.
- **macOS** loads the runtime `.so`/`.dylib` via `dlopen` and binds via `XR_EXT_cocoa_window_binding`. Current code is stubbed on Mac for the compositor path; validation tracked in [TODO.md](./TODO.md) §5.
- **Android** shares the runtime-loading pattern but validation is also pending; tracked in [TODO.md](./TODO.md) §5.

## Common gotchas for agents

- **Don't re-introduce "dual-path" framing.** If a task requires different behavior on Mac, use `#if PLATFORM_MAC` inside session/compositor, not a product flag.
- **Don't reach into `FDisplayXRSession` from components.** Use `FDisplayXRPlatform`. The session is thread-touchpoint-sensitive (compositor thread, game, render).
- **Don't block the compositor thread.** It must stay responsive to runtime vsync. Handshake is event-based; game/render work happens elsewhere.
- **Editor PIE ≠ standalone.** UE does not call `FDisplayXRDevice::UpdateViewport()` in PIE today — that's the whole point of the in-flight [EditorPreviewNative](./EditorPreviewNative.md) work.
