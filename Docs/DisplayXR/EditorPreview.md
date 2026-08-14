# Editor Preview — weaved 3D inside the PIE viewport tab

Pressing **Play** in the Unreal editor shows the DisplayXR runtime's **woven**
output directly inside the PIE viewport tab, driven through UE's own stereo
pipeline. No separate window, no second render path, no build step — the tab
can be floated like any editor tab, but that's an option, not a requirement.
This is parity with the [displayxr-unity](https://github.com/DisplayXR/displayxr-unity)
Game-view preview (its v2.8.0 weave-to-texture design).

Enabled by default (`r.DisplayXR.EditorNativePIE`, set `0` to disable).
**Windows only. Requires a runtime advertising `XR_DXR_display_zones`** — the
weave canvas comes from a display zone; without the extension the module warns
once at Play and the editor stays 2D.

## How it works

Per play session (`FDisplayXREditorModule` + `FDisplayXRPIEPreview`):

1. **Stereo on the PIE viewport.** `OnPostPIEStarted` forces
   `bEnableStereoRendering` on the game viewport widget so
   `FDisplayXRDevice` drives the PIE render — the same device path a packaged
   game uses. The flag is unforced at Stop (in "Selected Viewport" mode PIE
   borrows the level editor's own widget, which survives PIE).
2. **UE renders the SBS atlas into the viewport's own render target.** In this
   mode the device refuses every separate-render-target hook. This is
   deliberate and load-bearing: a docked stereo viewport that uses a separate
   RT flips Slate's window present onto its stereo-composite path, which
   renders the *entire editor window's UI* into the XR render target (that is
   UE's VR-headset design, and why Epic's VR Preview always spawns a
   standalone window). The PIE viewport is also unset from the window's
   registered-viewport slot for the duration, for the same reason.
3. **The atlas is handed to the runtime per frame.**
   `PostRenderViewFamily_RenderThread` acquires an OpenXR swapchain image,
   blits the tile region into it (XRBase's format-safe `AddXRCopyTexturePass`),
   and releases. One extra copy per frame, editor-only — the game path keeps
   the zero-copy swapchain-as-RT design.
4. **The runtime weaves into a shared texture.** The session is bound in
   texture mode: `XrWin32WindowBindingCreateInfoDXR.sharedTextureHandle` is a
   worst-case-sized shared D3D12 surface (runtime ADR-010: allocate once,
   never resize), and `windowHandle` is an **invisible, click-through proxy
   child window** glued over the viewport — it never presents; the display
   processor needs a real HWND to anchor interlace phase. One full-window
   `XrDisplayZoneDXR` rides `xrLocateViews` (zone-scoped locate — the rect IS
   the canvas) and the projection layer at `xrEndFrame`.
5. **The woven canvas is blitted into the tab.** A render-thread hook on
   `FSlateRenderer::OnBackBufferReadyToPresent` draws the woven texture 1:1
   into the backbuffer region under the viewport — after Slate painted the
   pre-weave atlas there, so the overwrite *is* the preview, and losing the
   hook degrades to the atlas rather than to nothing.

## Live tracking

The preview follows the viewport every editor tick: layout moves (splitters,
panel changes) apply immediately, size changes settle through a 0.35 s
debounce before the zone/proxy/blit update together (per-frame canvas resize
causes runtime swapchain-realloc storms), and moving the tab to another
top-level window restarts the preview against it (never `SetParent` on the
weaver-bound HWND — that collapses stereo permanently). Whole-window drags
cost nothing: the proxy is a child window whose screen position the display
processor tracks by itself. Canvas dimensions are forced even so the SBS tile
halving is exact across UE's view rects, the atlas copy, and the runtime's
sample rects.

## Session lifecycle

The XrSession outlives PIE, so every play session **rebinds** it (the
window binding and texture handle ride `xrCreateSession`). Expect ~1 s of
pre-weave SBS in the tab at Play while that happens. At Stop the compositor is
torn down while its bound proxy is still alive, the panel is handed back to 2D
(`xrRequestDisplayModeDXR`), and compositor creation stays disarmed until the
next Play so teardown races can't rebind against a dying window.

## Known limitations

- Editor backbuffers on 10-bit/HDR outputs get a one-shot WARN: the weave
  geometry is exact, colors may be slightly off until a PQ-aware encode pass
  exists.
- The proxy window's rect must stay in physical pixels; mixed-DPI multi-monitor
  layouts are untested.

## History

The shipped preview was previously `FDisplayXRPreviewSession` — a second,
standalone OpenXR session rendering via `USceneCapture2D` into its own
top-level window — plus an interim raw-Win32 "mirror window" experiment. Both
were deleted when this design landed; the archaeology (including the
Slate stereo-composite trap that shaped step 2) lives in
[`EditorPreviewNative.md`](./EditorPreviewNative.md) and
[issue #38](https://github.com/DisplayXR/displayxr-unreal/issues/38).
