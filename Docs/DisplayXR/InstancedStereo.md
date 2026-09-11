# Instanced Stereo (ISR)

How Unreal's instanced stereo rendering interacts with this plugin, what works, the
UE 5.7 engine bug that makes it crash, and the workaround the plugin applies.

Everything below was measured on 2026-09-11 with UE 5.7.4 (D3D12, SM6) on a
3840x2160 Leia panel using [`displayxr-unreal-test`](https://github.com/DisplayXR/displayxr-unreal-test)
launched as a game from the editor binary, plus a control run with UE's own
`OpenXRHMD` plugin against the same runtime. Run artifacts, crash dumps and the
harness are referenced at the end.

## TL;DR

- Instanced stereo **works** with this plugin: both eyes render in one pass into the
  2x1 side-by-side atlas, the swapchain handshake is unchanged, content reaches the panel.
- UE 5.7 **crashes within seconds** under instanced stereo whenever a sky light has
  *Real Time Capture* enabled. This is an engine bug, not a plugin bug (it reproduces
  with Epic's `OpenXRHMD`). In a Shipping build it looks like a black window and a
  runtime that never receives a frame.
- Since v0.9.2 the plugin forces `r.SkyLight.RealTimeReflectionCapture=0` when it detects
  instanced stereo, with a one-shot warning. `r.DisplayXR.InstancedStereoWorkarounds 0`
  opts out. Sky lights then use their captured cubemap instead of updating every frame.

## Turning it on

Instanced stereo is a **cook-time shader decision**, not a runtime toggle:

```ini
[/Script/Engine.RendererSettings]
vr.InstancedStereo=True
```

- The cvar is `vr.InstancedStereo`. There is no `r.InstancedStereo` in UE 5.7; that
  spelling is silently ignored.
- It is `ECVF_ReadOnly` and baked into every shader as the `INSTANCED_STEREO` define
  (`ShaderCompiler.cpp`), so changing it invalidates the whole shader DDC: the editor
  asks for a restart, and a packaged build must be fully recooked. The first `-game`
  launch after flipping it recompiles everything (about 8 minutes on a laptop RTX 3080).
- Ground truth is the startup log:

  ```
  LogInit: XR: Instanced Stereo Rendering is Enabled
  LogInit: XR: MultiViewport is Enabled
  ```

  If a build with ISR cooked in lands on a system that cannot do multi-viewport, the
  engine message-boxes `Cannot render an Instanced Stereo-enabled project` and exits.

The stereo device is never consulted: `FStereoShaderAspects` decides from the cvar and
the shader platform alone, so the plugin cannot veto ISR. It can only say what it
supports and work around what the engine gets wrong.

## What the plugin supports under ISR

UE's desktop ISR is **multi-viewport**: one 2D render target, two D3D12 viewports
selected by `SV_ViewportArrayIndex`, both eyes at `Y = 0`, primary view followed by its
secondary in the view family (`FSceneRenderer::SetStereoViewport` hard-codes `MinY = 0`
and pairs view N with view N+1). It is *not* a texture array; that is mobile multi-view.

The plugin's game path already satisfies that:

| Requirement | Plugin |
|---|---|
| `GetDesiredNumberOfViews(true) == 2` | 2 for the 2x1 layout the runtime reports for stereo panels |
| view 0 primary, view 1 secondary | inherited `GetViewPassForIndex` default |
| horizontally adjacent rects at `Y = 0` in one 2D target | `AdjustViewRect` tiles `Col * TileW, 0` |
| one 2D texture, not an array | wrapped swapchain image or UE's own atlas target |

Both atlas paths behave the same (`r.DisplayXR.UIPerEyeTiles` 1 and 0). Every
`FSceneViewExtension` callback the plugin relies on still fires for both eyes under ISR.

Not supported under ISR: any layout with more than two views or more than one tile row
(a runtime reporting a 2x2 or 1x2 layout). ISR would draw only the first two views and
force the second onto `Y = 0`. The plugin logs a warning naming the layout; set
`vr.InstancedStereo=False` for such displays.

Because the Unity sibling's single-pass-instanced path is a two-slice texture array
(UE's *mobile* multi-view equivalent), nothing from that implementation ports over.

## The engine bug

**Symptom.** With `vr.InstancedStereo=True`, every run crashed with
`EXCEPTION_ACCESS_VIOLATION` between 5 seconds and 2 minutes after the first frame,
9 of 9 runs, regardless of atlas path, Nanite single-pass, Lumen or virtual shadow maps.
UE's own `OpenXRHMD` (DisplayXR plugin disabled, runtime through the loader) crashed the
same way. Instanced stereo off: stable for as long as we ran it.

**Crash site.** Every minidump releases the same object: the reference controller of
`FSceneView::StereoCullingFrustum`, a `TSharedPtr<FConvexVolume>` that only exists
under single-pass stereo, after its block was already freed and reused. Sometimes the
game thread hits it in `FSceneView::~FSceneView` at the end of
`UGameViewportClient::Draw`; sometimes the asynchronous scene-renderer cleanup task hits
it in `~FViewInfo`; sometimes the renderer trips over a pointer the stale decrement
corrupted.

**Mechanism** (UE 5.7.4 source):

1. `ViewSnapshotCache::Create` (`Renderer/Private/Renderer/ViewSnapshotCache.cpp`)
   builds view snapshots with `FMemory::Memcpy(*Result, *InView)` and nulls only the
   uniform-buffer references afterwards. The snapshot therefore carries a bitwise,
   un-owned copy of `StereoCullingFrustum`.
2. `ReflectionEnvironmentRealTimeCapture.cpp`, `CreateMainViewSnapshotForRealTimeCapture`,
   takes such a snapshot of the main view, sets `StereoPass = eSSP_FULL`, then calls
   `CubeView.UpdateProjectionMatrix(...)`.
3. `FSceneView::UpdateProjectionMatrix` calls `SetupViewFrustum()`, which for a
   non-eye pass does `StereoCullingFrustum = nullptr` (`SceneView.cpp:824`). That
   assignment releases the shared pointer the snapshot never owned, decrementing the
   real controller's count.
4. The real owners, the game-thread `FSceneView` and the renderer's `FViewInfo`, release
   later; the second of them frees or dereferences a block that is already gone.

`WaterInfoTextureRendering.cpp` (Water plugin) does the same snapshot +
`UpdateProjectionMatrix` sequence and will have the same problem.

Threading knobs (`-norenderthread`, `-norhithread`, `r.SceneRender.CleanUpMode 0`) made
the crash disappear in five-minute runs, but only because they change whether the freed
block is reused before the last release. They hide the double release; they do not fix it.

**Engine fix** (for anyone building the engine from source): in
`ViewSnapshotCache::Create`, null the snapshot's `StereoCullingFrustum` (and
`ViewportFeedback`) shared pointers without release, the way it already does for
`ViewUniformBuffer` and `InstancedViewUniformBuffer`; or stop calling
`UpdateProjectionMatrix` on snapshots.

## The workaround

Real-time sky-light capture is the only shipping code path that calls
`UpdateProjectionMatrix` on a snapshot, and it is gated by a cvar
(`USkyLightComponent::IsRealTimeCaptureEnabled` checks
`r.SkyLight.RealTimeReflectionCapture`). With that cvar at 0, instanced stereo ran the
full five-minute test at about 48 fps, 14400 compositor frames, zero crashes, on the
default plugin path with no other change.

`FDisplayXRDevice` applies this at device creation, after the RHI is up and before any
world loads, whenever `FStereoShaderAspects` reports instanced stereo:

- logs that ISR is compiled in and which multi-viewport mode is active;
- warns if the runtime's tile layout is not 2x1;
- sets `r.SkyLight.RealTimeReflectionCapture` to 0 with `ECVF_SetByCode` (outranks
  scalability and device-profile writes) and logs a warning that names this document.

Opt out with `r.DisplayXR.InstancedStereoWorkarounds 0` (`ECVF_ReadOnly`, so in
`DefaultEngine.ini` `[ConsoleVariables]` or `-dpcvars`). The equivalent manual fix is to
untick *Real Time Capture* on the map's sky light, or to set the cvar yourself.

Visual cost: a Movable or Stationary sky light with Real Time Capture stops following
a dynamic sky and uses the cubemap captured at load; static sky lights are unaffected.

## Verifying a build

1. Startup log shows `XR: Instanced Stereo Rendering is Enabled`.
2. `LogDisplayXRDevice` shows `Instanced stereo is compiled in` followed by the
   `forcing r.SkyLight.RealTimeReflectionCapture=0` warning (unless opted out).
3. `LogDisplayXRCompositor: Compositor Thread: First atlas frame (... 2 views ...)` and
   `Compositor Thread: Frame N` advancing at the panel's refresh rate, with matching
   `AcquireColorTexture` / `ReleaseImage_RT` pairs.
4. In Development builds UE draws `StereoView: Primary` / `Stereo rendering method:
   Multi-viewport` in the game window; that is the engine's own ISR debug text.

## Evidence

Runs live under `C:\dxr-dev\runs\<label>` on the win box (harness
`C:\dxr-dev\run-isr.ps1`); crash dumps under the test project's `Saved/Crashes`.

| Run | Result |
|---|---|
| ISR off, plugin v0.9.1 | stable, 60 Hz, 5 min |
| ISR on, per-eye UI path (default) | crash, 5 to 110 s, 6/6 |
| ISR on, zero-copy path (`UIPerEyeTiles 0`) | crash after 20 s |
| ISR on, Nanite one-pass off / Lumen off / VSM off | crash every time |
| ISR on, UE `OpenXRHMD`, DisplayXR plugin disabled | crash after 90 s, same signature |
| ISR on, `-norenderthread` / `-norhithread` / `r.SceneRender.CleanUpMode 0` | no crash in 5 min (heap luck, see above) |
| ISR on, `r.SkyLight.RealTimeReflectionCapture=0` | stable, 5 min, 14400 frames |

Dumps were read with `cdb` against export symbols (no editor PDBs installed); the
release site is `FSceneView::~FSceneView+0x1c6` operating on the member at
`FSceneView+0x48`, which is `StereoCullingFrustum`'s controller pointer.

## Related plugin work (not part of the crash)

- The device never sets `EngineShowFlags.StereoRendering`, so `SetFinalViewRect` is
  never called and display-based screen percentage stays enabled for a stereo family.
- The compositor re-measures the window to compute projection-layer image rects instead
  of consuming the rects UE actually rendered.

Both are tracked in [TODO.md](./TODO.md).
