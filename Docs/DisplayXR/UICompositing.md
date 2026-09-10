# 2D UI (UMG / Slate) on a stereo display — per-eye tile compositing

Design note for the game-path UI fix (`r.DisplayXR.UIPerEyeTiles`, default on).

## The defect

With a stereo device active and a separate render target, UE's Slate renderer takes its
stereo-composite path (`FSlateRHIRenderer::DrawWindow_RenderThread`, `bCompositeStereoToSwapChain`):
it paints the **whole window's UI once into the viewport render target** and then calls
`IStereoRendering::RenderTexture_RenderThread` so the device can finish the frame.

On the v0.7.0 game path the viewport render target *is* the OpenXR swapchain image holding both
eye tiles (zero-copy). The tiles are `window × view_scale` (0.5 × 0.5 here) at the top-left of the
panel-sized image, so a window-sized HUD lands across both tiles: the left eye gets the left part
of the HUD, the right eye the right part, and the weave scales each tile up to the panel. Any
project with a UMG HUD sees a doubled-size, half-transparent, half-missing UI.
`RenderTexture_RenderThread` was a no-op, so nothing corrected it, and the UI was also painted
*after* the image had been released to the runtime.

## The fix — where the UI is isolated, and what it costs

Slate can only paint into the viewport's render target, and that is also where the scene lands,
so the UI cannot be redirected without an engine change. Instead the frame is split in two:

1. **UE renders the atlas into its own, window-sized render target** (`AllocateRenderTargetTextures`
   declines the swapchain images; the existing singular allocator provides a `B8G8R8A8` target
   sized by `CalculateRenderTargetSize` to the window client rect). The tiles (`window ×
   view_scale`, top-left) always fit inside it and keep their rects, so `AdjustViewRect` and the
   swapchain copy are unchanged. The target must be window-sized: Slate paints with a window-sized
   projection but the target's full extent as the viewport, while its clipping scissors stay in
   window pixels — on a panel-sized target the UI comes out stretched by `extent / window` and
   clipped to the top-left window rect (only the UI's top-left quarter survives in a 1920x1080
   window on a 3840x2160 panel; a fullscreen window hides this, since window == panel).
2. **`PostRenderViewFamily_RenderThread`** — after all scene passes, before Slate paints —
   acquires a swapchain image, copies the tile region (the union of the view rects, same rect on
   both sides) into it, and **clears the render target to transparent black**. The image is *not*
   released yet.
3. **Slate paints the window UI** 1:1 into the now-transparent render target. Slate's element
   blend (`SrcAlpha / InvSrcAlpha`, alpha accumulates coverage) over `(0,0,0,0)` leaves a
   premultiplied UI layer covering the whole (window-sized) target.
4. **`RenderTexture_RenderThread`** blits that layer into **every eye tile** with
   `EXRCopyTextureBlendModifier::PremultipliedAlphaBlend` (XRBase `AddXRCopyTexturePass`, which
   handles the scale), then releases the swapchain image. If Slate skipped a window paint, the
   next `PostRenderViewFamily` releases the still-held image untouched.

The UI is identical in both eyes at the same tile coordinates, so it sits at **zero disparity, the
screen plane** — where a HUD belongs on a light-field display.

Cost: **one copy of the tile area per frame** (half the window area at 0.5 scale) plus one clear
and two scaled alpha blits of the UI layer. Zero-copy for the scene is given up on this path;
it is the same hand-off the editor in-tab preview already uses (`Atlas copy`), and it is the
minimum — Slate must paint somewhere that is not the final atlas. `r.DisplayXR.UIPerEyeTiles 0`
restores the v0.7.0 zero-copy flow (read at startup: `DefaultEngine.ini [ConsoleVariables]` or
`-dpcvars=r.DisplayXR.UIPerEyeTiles=0`). Editor texture mode and the IPC array-copy path are
unaffected and keep their existing flows.

### Where the target size comes from

`Viewport.GetSizeXY()`, clamped to the swapchain. Two other sources were tried and are wrong:
`CacheWindowSize()` prefers the compositor's bound overlay, whose size the runtime derives from
what the app renders (a feedback loop), and `GameHWND` is cleared by `UpdateViewport` on frames
that carry no viewport widget, so measuring it falls back to the panel on those frames. Either way
the target alternated between window and panel size every few frames, reallocating constantly and
tripping D3D12's scissor-vs-viewport `ensure` (`D3D12StateCache.cpp`, `bScissorRectValid`). The
viewport's own size has no such dependency and is exactly the rect Slate lays the UI out in.

The target still reallocates a few times while a level loads, as the viewport settles on its size;
it is stable in steady state and tracks a window resize in one step. Tiles are clamped to both
extents in the copy, so a transitional frame never samples outside either texture.

One transitional frame per resize is imperfect, though. `NeedReAllocateViewportRenderTarget` is how
UE notices the size changed and it runs on the frame *after* the change, so that frame is still
drawn into the previous target while Slate lays the UI out at the new window size — the same
projection-vs-extent mismatch described above, scaled by `new window / old target` and clipped to
the new window rect. The UI can therefore jump for a single frame mid-resize. Not observed by eye
(one frame, only while dragging) and the steady state either side is correct, so it is recorded
rather than worked around; the settle-debounce this plugin already uses for the editor preview's
canvas is where to start if it matters.

## Input and hit-testing (CommonUI, Lyra)

Nothing changes. Slate lays out and hit-tests in **window pixels**; the UI is painted 1:1 into
the window-sized target, the blit maps it onto each tile at `tile / window` scale, and the runtime
scales each tile back up to the window's client rect when it weaves — the maps compose to
identity. A widget therefore appears at the window position Slate thinks it has, so mouse
hit-testing, CommonUI's input routing, analog cursors and focus all keep working. The cursor is a
Slate element too, so it renders in both eyes at the screen plane — the same plane the hit test
happens in. Input forwarding from the overlay child window (`OverlayProc`) is unchanged. Verified
in Lyra: hovering and clicking the pause-menu and settings widgets responds at the cursor, both
windowed and fullscreen.

Resolution caveat: the UI ends up at tile resolution (half the window at 0.5 scale) before the
weave scales it back up, so text is softer than in 2D. A full-resolution UI layer would need
runtime support (see Local2D below).

## Alternatives considered

- **UE `IStereoLayers` (quad layers)** — the plugin would implement a layer manager and
  composite face-locked quads into each eye. It handles depth-placed panels well, but UMG "Add to
  Viewport" content is not captured: games must render their UI to a texture and route input
  themselves, which rules it out for an existing HUD such as Lyra's. Worth adding later for
  panels that want depth.
- **Runtime Local2D layer** — submit the UI as a 2D layer and let the runtime overlay it
  un-woven at full panel resolution. The region under a Local2D layer is flattened to 2D, so it
  suits menus and panels, not a full-screen HUD that overlaps the 3D scene everywhere.
