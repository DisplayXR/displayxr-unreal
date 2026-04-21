# ADR-002 — Zero-copy atlas handoff via single D3D12 device

**Status:** Accepted
**Date:** 2026-04

## Context

The DisplayXR runtime wants an OpenXR swapchain it can weave against. UE has its own render target pipeline. The naive integration is: let UE render into a UE-owned render target, then copy to the OpenXR swapchain image each frame. That works but:

- Burns bandwidth on a 4K+ backbuffer every frame.
- Needs synchronization we can get wrong (flicker, torn frames).
- On cross-process runtimes, the copy has to go through shared-handle plumbing and fences — even more friction.

We did build an intermediate version of that (separate device + shared texture + cross-process copy) before landing the current design. It worked but was complex and fragile — documented in [CompositorIntegration.md](../CompositorIntegration.md) as historical context.

## Decision

UE renders **directly** into the OpenXR swapchain. The flow:

1. `FDisplayXRCompositor::Initialize()` takes UE's D3D12 device and command queue (borrowed, not owned) plus a dedicated **runtime queue** created on UE's device.
2. `xrCreateSession` uses the runtime queue as its `XrGraphicsBindingD3D12KHR`. UE and the runtime share one D3D12 device — no cross-device copy, no shared handles.
3. `xrCreateSwapchain` with UE's preferred format. The resulting `ID3D12Resource*` images are wrapped as `FRHITexture` via the D3D12 RHI's external-resource wrap path.
4. UE's `AllocateRenderTargetTexture` returns a wrapped swapchain texture from the current pool. UE's renderer writes directly into it.
5. A dedicated compositor thread runs the frame handshake (`xrWaitFrame` / `xrBeginFrame` / `xrEndFrame`) off UE's game/render threads, coordinated via two `FEvent`s.

## Alternatives considered

1. **Copy each frame.** Rejected for bandwidth and sync reasons above.
2. **Separate D3D12 device for the runtime, cross-device shared textures.** Built as an intermediate. Dropped once the single-device path was proven — it's strictly simpler and faster.
3. **Let UE's `FOpenXRHMD` own the swapchain.** Blocked by ADR-001 (we don't use UE's OpenXR plugin).
4. **Run the compositor on UE's render thread.** Rejected: `xrWaitFrame` blocks on runtime vsync and would stall UE's frame timing. The dedicated thread keeps the runtime pacing independent of UE's scene work.

## Consequences

- No per-frame copies. UE's scene output *is* the OpenXR submission.
- Requires a D3D12 path; the zero-copy version is Windows-only. Mac/Metal uses a different handoff strategy (tracked in [TODO.md](../TODO.md)).
- The compositor thread is an extra thread you need to reason about when touching session state — the session's tunables and view config are already double-buffered with atomic index swaps for that reason.
- Format and extent must be negotiated with both UE's preferred backbuffer format and the runtime's supported list. Failure mode: runtime rejects the swapchain create. Detailed in [AtlasHandoff.md](../AtlasHandoff.md).

## Related

- [AtlasHandoff.md](../AtlasHandoff.md) — implementation detail of the wrap + handshake
- [CompositorIntegration.md](../CompositorIntegration.md) — prior "separate device + IPC" history
