# DisplayXR Compositor Integration (Phase 2)

## Current State (Phase 1 — Working)

UE renders an N-view atlas directly to the backbuffer via IStereoRendering:
- `GetDesiredNumberOfViews(2)` → UE renders 2 views
- `AdjustViewRect` → tiles placed per ViewConfig (2×1, scale 0.5×0.5)
- `GetStereoProjectionMatrix` → Kooima off-axis projection per view
- `CalculateStereoViewOffset` → per-view eye displacement
- Display: 3840×2160 physical pixels, atlas 3840×1080 (top half)

The atlas is visible on screen but NOT interlaced/weaved — the compositor
is not connected.

## What's Needed for Weaved Output

The DisplayXR compositor reads from an **OpenXR swapchain**, not from HWND
capture. The atlas must be submitted via `xrEndFrame` with a
`XrCompositionLayerProjection` referencing a swapchain.

### Architecture (from Unity plugin reference)

```
UE renders atlas to backbuffer
    │
    ▼  (GPU copy per frame)
OpenXR swapchain image (atlas texture)
    │
    ▼  xrEndFrame with XrCompositionLayerProjection
DisplayXR compositor (in-process)
    │
    ▼  interlacing / weaving
3D display output via child window
```

### Key Components

1. **Load runtime directly** (not via openxr_loader.dll → IPC client)
   - Use `xrNegotiateLoaderRuntimeInterface` to get `xrGetInstanceProcAddr`
   - Load the runtime DLL from the path in `XR_RUNTIME_JSON`
   - This gives the in-process compositor, not the IPC service client

2. **Child window overlay**
   - Cannot create two D3D12 swap chains on the same HWND
   - Create a transparent child window on UE's game window
   - The compositor outputs to this child window
   - Unity does this via `displayxr_get_app_main_view()`

3. **OpenXR swapchain**
   - Create `XrSwapchain` at atlas dimensions (e.g., 3840×1080)
   - Each frame: acquire image → GPU copy atlas from UE backbuffer → release
   - Format: `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` or matching UE's backbuffer

4. **Frame submission**
   - `xrWaitFrame` → `xrBeginFrame` → render → `xrEndFrame`
   - Submit `XrCompositionLayerProjection` with views referencing atlas tiles
   - Each view's `subImage.imageRect` = tile position within atlas
   - Must run on a thread that can block (xrWaitFrame blocks for vsync)

5. **D3D graphics binding**
   - Session needs `XrGraphicsBindingD3D12KHR` with UE's device + queue
   - Must call `xrGetD3D12GraphicsRequirementsKHR` before session creation

### Implementation Plan

1. **FDisplayXRCompositor class** — owns the compositor lifecycle:
   - Loads runtime DLL directly (negotiate pattern)
   - Creates child window on UE's game window
   - Creates XrSession with D3D12 binding + Win32 window binding
   - Creates XrSwapchain at atlas dimensions
   - Per-frame: copies atlas → submits via xrEndFrame

2. **Integration with FDisplayXRDevice**:
   - `UpdateViewport` → creates compositor when HWND is available
   - `RenderTexture_RenderThread` or post-render callback → triggers atlas copy
   - Compositor runs its frame loop (possibly on a separate thread)

3. **Threading considerations**:
   - `xrWaitFrame` blocks — cannot run on UE game thread
   - Options: dedicated compositor thread, or skip xrWaitFrame and accept tearing
   - Unity plugin hooks into the engine's frame submission, not a separate thread

### Reference Code

- **Unity plugin**: `C:\Users\Sparks i7 3080\Documents\GitHub\unity-3d-display\native~\`
  - `displayxr_standalone.cpp` — runtime loading, session creation
  - `displayxr_d3d11_backend.cpp` — atlas composition into swapchain
  - `displayxr_hooks.cpp` — xrEndFrame interception, layer patching
  - `displayxr_win32.h/.c` — child window creation
- **Test app**: `C:\Users\Sparks i7 3080\Documents\GitHub\openxr-3d-display\test_apps\cube_handle_d3d12_win\`
  - `xr_session.cpp` — session creation with D3D12 + Win32 binding
  - `main.cpp` — frame loop, layer submission
- **Extension docs**: `C:\Users\Sparks i7 3080\Documents\GitHub\openxr-3d-display\docs\specs\`

### Current Session Status

The session CAN be created with D3D12 binding (verified working):
```
D3D12 graphics requirements queried (minFeatureLevel=45056)
Creating session with D3D12 device=... queue=... hwnd=...
Mode[0] '2D' views=1 scale=1.00x1.00 tiles=1x1 viewPx=3840x2160 hw3D=0
Mode[1] '3D' views=2 scale=0.50x0.50 tiles=2x1 viewPx=1920x1080 hw3D=1
Session created with graphics binding
Session running
```

But this uses the IPC client (DisplayXRClient.dll via openxr_loader.dll).
For weaving, need the in-process compositor loaded directly.
