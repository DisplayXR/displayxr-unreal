# Atlas Handoff — UE Renders Directly Into the OpenXR Swapchain

This document describes how UE's rendered atlas reaches the DisplayXR runtime's
OpenXR swapchain. It is a **zero-copy** pipeline: UE renders its N-view atlas
tiles directly into OpenXR swapchain images (wrapped as `FRHITexture`s), and
the DisplayXR compositor reads them for display composition. No cross-device
copies, no shared textures, no cross-device fences.

## The Problem

UE renders an N-view atlas (e.g., 3840×2160 with 2×1 tiles of 1920×1080 in the
upper half) into a render target. The DisplayXR runtime's compositor reads
from an **OpenXR swapchain** — a set of D3D12 textures owned by the runtime.

In the naive approach the two live on separate D3D12 devices, requiring shared
textures and cross-device sync to ferry pixels between them. The zero-copy
pipeline described here avoids that entirely by hosting both on UE's D3D12
device and using UE's native `IStereoRenderTargetManager` API
(`AllocateRenderTargetTextures` + `AcquireColorTexture`) to make each
swapchain image look like an ordinary UE render target.

## Working Solution

### Pipeline Overview

```
Game thread (UE)              Render thread (UE)             Compositor thread
────────────────              ──────────────────             ──────────────────
AcquireColorTexture():                                        xrWaitFrame   (vsync block)
  wait BeginFrameReady                                        xrBeginFrame
  xrAcquireSwapchainImage    ← triggered by →                 Trigger BeginFrameReady
  xrWaitSwapchainImage                                        Wait EndFrameReady (500ms)
  return idx
  ↓
UE scene renders directly
into SwapchainImages[idx]
                                UE RDG flushes
                                ↓
                              PostRenderViewFamily_RenderThread:
                                Transition → Present
                                xrReleaseSwapchainImage
                                Trigger EndFrameReady  →       wake
                                                                ↓
                                                              xrEndFrame
                                                                with projection
                                                                layer
                                                              Reset BeginFrameReady
                                                              loop
```

No GPU copies. The swapchain image IS the render target. UE draws the atlas
tiles into it in place.

### D3D12 Device Topology

```
UE's D3D12 Device (single device for everything)
─────────────────────────────────────────────────
UE's main command queue                     RuntimeQueue
  (UE rendering)                             (given to xrCreateSession)
                                             (the runtime's own submissions)

Swapchain[0..N-1]  (owned by the runtime, allocated on UE's device)
   ↑ wrapped as FTextureRHIRef via RHICreateTexture2DFromResource
   ↑ returned to UE by AllocateRenderTargetTextures
```

The runtime receives a **dedicated** `ID3D12CommandQueue` created on UE's
device, not UE's main queue. UE's RHI state tracking does not tolerate
foreign submissions on its own queue; giving the runtime its own queue
keeps them isolated while still sharing the device.

### Key Design Decisions

| Decision | Rationale |
|---|---|
| **Single D3D12 device (UE's)** for the OpenXR session | Eliminates shared-texture bridge and cross-device fence. The runtime allocates swapchain images on UE's device, making them directly usable as UE render targets. |
| **Dedicated `RuntimeQueue` on UE's device** given to `xrCreateSession` | The runtime issues its own `ExecuteCommandLists` work; putting it on UE's main queue corrupts UE's RHI tracking (descriptor heaps, fence values). A second queue on the same device keeps submissions isolated while sharing GPU memory. |
| **Swapchain at full display resolution (3840×2160)** | Required by the DisplayXR runtime's compositor. Tiles are sub-rects within each swapchain image. Creating at atlas size (3840×1080) triggers GPU device removal. |
| **`RHICreateTexture2DFromResource(PF_B8G8R8A8, …, Resource)` per swapchain image** | Produces an `FTextureRHIRef` that wraps the runtime's `ID3D12Resource`. UE treats it like any other render target. Flags match UE's `FOpenXRHMD`: `RenderTargetable | ShaderResource | ResolveTargetable | Dynamic`. `FClearValueBinding::Transparent` is the neutral choice. |
| **`AllocateRenderTargetTextures` (plural) returns the wrapped array** | UE 5.7 `IStereoRenderTargetManager` defines both the plural allocator and `AcquireColorTexture`. Combined, they are exactly the OpenXR swapchain shape. `FSceneViewport` already knows how to use them. |
| **`AcquireColorTexture` called on the game thread** | Matches UE's own `FOpenXRHMD` pattern (see `OpenXRHMD.cpp:2725`). `FSceneViewport::EnqueueBeginRenderFrame` invokes it once per frame, then selects `BufferedRenderTargetsRHI[idx]` as the scene RT. |
| **`ShouldUseSeparateRenderTarget() = true`** | Required for UE 5.7 to take the separate-RT path at all. Without this UE does not route through the render-target-manager hooks. |
| **Swapchain image release happens in `PostRenderViewFamily_RenderThread`** | `RenderTexture_RenderThread` only runs for the spectator-screen blit path and was never reached by our scene graph. `PostRenderViewFamily_RenderThread` is the SceneViewExtension hook that fires inside the scene's RDG graph, after all scene passes, and is the correct place to release the swapchain image. |
| **Explicit `ERHIAccess::Unknown → Present` transition before `xrReleaseSwapchainImage`** | UE leaves the swapchain texture in a scene-color state after rendering. The OpenXR runtime expects the image in a presentable layout before it takes over. |
| **Compositor thread owns `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`, nothing else** | `xrWaitFrame` blocks for vsync; running it on UE's game thread would gate everything else at vsync rate, including tick. The compositor thread isolates that block and does no GPU work. |
| **Per-frame handshake via two `FEvent`s** | `BeginFrameReadyEvent` (manual-reset, compositor → game) ensures UE acquires only after the runtime has a frame to hand out. `EndFrameReadyEvent` (auto-reset, render → compositor) ensures the runtime's `xrEndFrame` is called after UE has released the image. |
| **`bImageAcquiredThisFrame` owned exclusively by the Release path (via `CompareExchange`)** | Earlier iterations cleared this flag in the compositor's timeout path, which let Release silently skip while the image stayed acquired in the runtime. After three frames all swapchain images were leaked and `xrAcquireSwapchainImage` returned `-37` (CALL_ORDER_INVALID). Keeping the flag single-owner makes this impossible. |
| **Compositor's `EndFrameReadyEvent->Wait` uses 500 ms, not 50 ms** | UE's render thread can lag the compositor by more than one vsync interval under load. A short timeout caused the compositor to submit empty frames while Release was still in flight, breaking the 1:1 Acquire/Release pairing. |
| **Defensive re-acquire guard in `AcquireColorTexture`** | If UE somehow calls `AcquireColorTexture` a second time before Release has run (e.g., because `PostRenderViewFamily_RenderThread` was delayed), we return the cached index instead of calling `xrAcquireSwapchainImage` again. Re-acquiring exhausts the swapchain pool. |
| **`Session->SetPredictedDisplayTime()` published from the compositor thread** | Game-thread `xrLocateViews` needs the `predictedDisplayTime` from the most recent `xrWaitFrame`; without it `xrLocateViews` returns `XR_ERROR_TIME_INVALID (-30)` and eye tracking freezes at the fallback. |

### Files

| File | Role |
|---|---|
| `DisplayXRCompositor.h` | Class declaration, swapchain-image wrapping, handshake events |
| `DisplayXRCompositor.cpp` | Runtime queue creation, swapchain creation + wrapping, Acquire/Release entry points, compositor frame loop |
| `DisplayXRDevice.h` | HMD device with plural `AllocateRenderTargetTextures` + `AcquireColorTexture`, `PostRenderViewFamily_RenderThread` |
| `DisplayXRDevice.cpp` | Allocator + Acquire implementations, swapchain-sized render target sizing, one-shot reallocation trigger, release pass |
| `DisplayXRSession.{h,cpp}` | OpenXR session, runtime loading via negotiate, `PredictedDisplayTime` atomic consumed by `xrLocateViews` |

## Per-Frame Handshake

The game, render, and compositor threads run at different cadences and must
not violate the two OpenXR invariants:

1. Every `xrAcquireSwapchainImage` must be followed by exactly one
   `xrReleaseSwapchainImage` before the next `xrAcquire` for the same
   swapchain index.
2. `xrBeginFrame`/`xrEndFrame` must be paired 1:1.

The handshake keeps them paired:

| Event | Signals from | Signals to | Reset by | Reset mode |
|---|---|---|---|---|
| `BeginFrameReadyEvent` | compositor (after `xrBeginFrame`) | game thread (`AcquireColorTexture`) | compositor (after `xrEndFrame`) | manual |
| `EndFrameReadyEvent` | render thread (`ReleaseImage_RenderThread`) | compositor (before `xrEndFrame`) | auto-reset on `Wait` | auto |

Additional state:

- `TAtomic<bool> bImageAcquiredThisFrame` — set by `AcquireColorTexture`,
  atomically claimed+cleared by `ReleaseImage_RenderThread` via
  `CompareExchange`. The compositor thread **reads** it but never writes it.
- `TAtomic<uint32> AcquiredImageIndex` — published by `AcquireColorTexture`
  so the defensive re-acquire guard can return the same index.
- `TAtomic<int64> LastPredictedDisplayTime` — diagnostic record of the last
  `xrWaitFrame` prediction, for logging.

The fallback paths are self-healing. If the game thread never acquires for a
compositor frame, the compositor times out on `EndFrameReadyEvent->Wait` and
submits `xrEndFrame` with `layerCount = 0`, which is valid per spec. If the
render thread is delayed past the 500 ms window, Release still executes on
its own schedule (independent of the compositor's timeout) and the pipeline
catches up on the next frame.

## Why This Works (Invariants)

- **The runtime only sees UE's device.** No shared-handle dance, no
  cross-adapter checks, no second device to keep in lockstep.
- **The runtime's submissions never land on UE's queue.** The dedicated
  `RuntimeQueue` is the only queue handed to `xrCreateSession`, and we never
  submit UE work to it.
- **Every `Acquire` has exactly one `Release`.** The atomic
  `bImageAcquiredThisFrame` makes Release idempotent (subsequent claims are
  no-ops) and makes Acquire idempotent within a frame (re-entry returns the
  cached index).
- **Every `xrBeginFrame` has exactly one `xrEndFrame`.** The compositor loop
  unconditionally calls `xrEndFrame` — with a layer on the success path,
  with zero layers on the timeout path.
- **`ShouldUseSeparateRenderTarget` never lies.** It always returns `true`,
  so UE's code path through `FSceneViewport::EnqueueBeginRenderFrame`
  consistently calls `AcquireColorTexture` each frame.
- **Swapchain size is always 3840×2160** (full display), never atlas size.
  Tile-sub-rect positioning in `AdjustViewRect` + the projection layer's
  `subImage.imageRect` places the N views in the upper 3840×1080 of the
  swapchain image.

## Approaches Considered but Rejected

These were evaluated (some empirically, some against UE's source) and
discarded before or during implementation. Recorded here so they don't need
to be re-evaluated.

### Separate D3D12 device for the compositor + shared texture bridge

Two GPU copies per frame (atlas → shared texture on UE's device → swapchain
on compositor's device), a cross-device fence, and a second `ID3D12Device`
on the same adapter. Correct but wasteful: two copies, twice the GPU memory
footprint for the bridge, plus the complexity of sharing `NTHANDLE`s and
synchronizing two queues. Rejected because the single-device zero-copy path
works.

### Submit raw D3D12 commands on UE's main queue

Directly calling `ExecuteCommandLists` on UE's queue (either for the
atlas→shared copy, or as the queue passed to `xrCreateSession`). Corrupts
UE's internal RHI state tracking (open command lists, fence values,
descriptor heaps) and causes GPU device removal on the next UE submission.
Always use a dedicated queue.

### `ShouldUseSeparateRenderTarget() = false`

UE never calls `RenderTexture_RenderThread` — or any render-target-manager
hook — when this returns false. The plugin would have to render into UE's
backbuffer directly. No viable hook for swapchain integration.

### Use `AllocateRenderTargetTexture` (singular, not plural) and rotate the underlying resource per frame

Swapping the `ID3D12Resource*` behind an existing `FRHITexture` breaks UE's
RHI state tracking (resource transitions assume identity-stable textures).
Using the plural allocator + `AcquireColorTexture` is UE's intended OpenXR
shape and avoids all of this.

### Release in `RenderTexture_RenderThread`

The function is called by Slate for the spectator-blit step and does not
fire inside the scene's RDG graph. Passes added there never executed in our
configuration. `PostRenderViewFamily_RenderThread` (a SceneViewExtension
hook) runs inside the scene graph and is the correct hook — it is also
guaranteed to fire once per view family.

### Letting the compositor thread clear `bImageAcquiredThisFrame` on timeout

The compositor timing out while UE is still rendering, then clearing the
flag, causes Release to skip (thinks no image is acquired). The image stays
acquired in the runtime's view forever. After three frames all swapchain
images are leaked and `xrAcquireSwapchainImage` returns
`XR_ERROR_CALL_ORDER_INVALID (-37)`. Release must own the flag exclusively.

### Short timeout (50 ms) on `EndFrameReadyEvent->Wait`

UE's render thread can take longer than a vsync interval under load.
Compositor timeouts while UE is still rendering race the state machine. 500
ms is comfortably above any realistic UE frame cost but low enough that a
truly stalled render thread doesn't hang the compositor.

### Format mismatch R10G10B10A2 ↔ B8G8R8A8

UE's default HDR render target is R10G10B10A2 (DXGI 24, `PF_A2B10G10R10`);
swapchains the DisplayXR runtime advertises are B8G8R8A8 (DXGI 87). The
zero-copy path forces B8G8R8A8 end-to-end via
`AllocateRenderTargetTextures(PF_B8G8R8A8, …)` and
`GetActualColorSwapchainFormat() == PF_B8G8R8A8`. Omitting either one
leaves some part of the pipeline in R10G10B10A2 and breaks rendering to the
wrapped swapchain image.

## Comparison With Reference Implementations

Three other implementations get an atlas into a DisplayXR compositor. Our
approach matches the "direct render into swapchain" pattern used by the
D3D12 test app.

| | **UE Plugin (this doc)** | **Unity Standalone D3D12** | **Unity Hooks (in-app)** | **D3D12 Test App** |
|---|---|---|---|---|
| **D3D12 device** | UE's device + dedicated runtime queue | Own device (separate from Unity) | Unity's device | Own device (app is the renderer) |
| **Atlas → swapchain** | Direct render into swapchain | Copy via shared texture (`blit_atlas`) | No copy — runtime hooks `xrEndFrame` | Direct render into swapchain |
| **Number of copies** | **0** | 1 (bridge → swapchain) | 0 | 0 |
| **Threading** | Dedicated compositor thread (xrWaitFrame only); game-thread Acquire; render-thread Release | Single thread (Unity render thread) | Single thread (Unity render thread) | Separate render thread |
| **Sync** | Two `FEvent`s + atomic flag | Blocking fence per blit (`WaitForSingleObject`) | Implicit (hook runs inline) | Blocking acquire/release |
| **Swapchain size** | Full display (3840×2160) | Runtime-determined | Runtime-determined | Tile-based atlas size |
| **Format control** | Forced B8G8R8A8 via `AllocateRenderTargetTextures` + `GetActualColorSwapchainFormat` | Format from `xrEnumerateSwapchainFormats` | N/A | Format from `xrEnumerateSwapchainFormats` |

### D3D12 Test App — Direct Render

The test app owns the rendering pipeline end-to-end. It creates a D3D12
device, passes it to `xrCreateSession`, and renders directly into swapchain
images:

```
xrAcquireSwapchainImage → get RTV for image[idx]
→ record draw commands targeting swapchain texture
→ execute + xrReleaseSwapchainImage → xrEndFrame
```

Our pipeline is the same shape with UE's renderer sitting where the test
app's draws would be. The difference is that we don't own the render
pipeline — UE does — so we surface the swapchain images via
`AllocateRenderTargetTextures` + `AcquireColorTexture` and let UE's
renderer draw into them.

### Unity Standalone D3D12 — Shared-Texture Bridge

Unity's standalone backend creates its own D3D12 device and bridges via a
shared texture (one copy per frame). Simpler to reason about but it has a
synchronous `WaitForSingleObject` per blit and doubles GPU memory. Unity
doesn't expose an API equivalent to UE's `AllocateRenderTargetTextures`, so
the bridge is the cheapest correct option there.

### Unity Hooks (In-App)

Hooks Unity's OpenXR plugin's `xrEndFrame` and injects composition layers
referencing Unity's own swapchain. Zero copies, no plugin-side swapchain,
but requires Unity's OpenXR stack to be compatible with the runtime's
extensions. UE's `FOpenXRHMD` would need the equivalent treatment — fragile
and version-dependent — so we replace it with our own `FHeadMountedDisplayBase`
instead (see the DisplayXR architecture doc).

## Verification

Build with `build.bat`, run with `run-game.bat`. Expected log signals:

- `Compositor: RuntimeQueue created on UE device (<ptr>)`
- `Compositor: Swapchain 3840x2160 fmt=87 (3 images)`
- `Compositor: Wrapped 3 swapchain images as RHI`
- `Compositor: Initialized (single-device, zero-copy mode; swapchain deferred)`
- `Compositor: Ready`
- `Compositor Thread: Started`
- `DisplayXR: AllocateRenderTargetTextures -> 3 swapchain images (3840x2160)`
- `AcquireColorTexture #1 -> idx=0`, `#2 -> idx=1`, `#3 -> idx=2`, `#4 -> idx=0`, … (rotating through all three)
- `ReleaseImage_RT #N (xr=0)` for every Acquire
- `Compositor Thread: First atlas frame (frame 2, 2 views 1920x1080, xrEndFrame=0)`
- `Compositor Thread: Frame 300`, `Frame 600`, … (every 300 frames, stable)
- Periodic `LocateViews #N: result=0 views=2 flags=0xf posValid=1 posTracked=1 displayTime=<large>` from the session — parallax driven by real `predictedDisplayTime`

Expected **absent**:

- `CopyAtlas` (no GPU copies)
- `fence signaled val=…` (no cross-device fence)
- `Shared texture` (no shared textures)
- `Own D3D12 device` (no second compositor device)
- `xrAcquireSwapchainImage failed -37` (no swapchain-image leaks)
- `DEVICE_REMOVED`, `EXCEPTION_ACCESS_VIOLATION`, GPU crash reports

Visual:

- Weaved/interlaced 3D output on the physical display.
- Parallax tracks head movement (eye positions come from `xrLocateViews`,
  which requires the `predictedDisplayTime` plumbing above).

Stability target:

- 300+ frames (`Compositor Thread: Frame 300`) is the minimum "session held";
  4800+ frames at 60 fps with no errors is the observed steady state.
