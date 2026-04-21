# Quick Start

Install the runtime → install the plugin → open a UE project → press Play. ~10 minutes.

For Mac-specific quirks (OpenXR active-runtime switching, no UE OpenXR plugin), also read [MacSetup.md](./MacSetup.md).

## 1. Install the DisplayXR OpenXR runtime

The plugin is useless without the runtime. Install from [openxr-3d-display](https://github.com/dfattal/openxr-3d-display) — that repo's README has the current install steps per platform.

Verify the runtime is discoverable:

```
# Windows
reg query "HKLM\SOFTWARE\Khronos\OpenXR\1" /v ActiveRuntime

# macOS / Linux
cat /etc/xdg/openxr/1/active_runtime.json
```

Either should point to a `openxr_displayxr.json` manifest and a corresponding `.dll` / `.so` / `.dylib`.

## 2. Set up a UE project

1. Create a new Unreal Engine 5.3+ C++ project (Blueprint-only projects can't compile plugins — use C++ Blank).
2. Close the editor.
3. Clone this repo into `Plugins/DisplayXR/`:
   ```
   cd <YourProject>
   mkdir -p Plugins
   git clone https://github.com/DisplayXR/displayxr-unreal.git Plugins/DisplayXR
   ```
4. Right-click the `.uproject` → **Generate Visual Studio project files** (Windows) or regenerate Xcode project (Mac).
5. Build from your IDE, or double-click the `.uproject` to let UE compile it.

On first launch the editor will report `DisplayXR: Session initialized` in the Output Log. If it reports `Failed to initialize session`, the runtime install from step 1 didn't take — fix that before going further.

## 3. Set up a rig in the scene

Simplest camera-centric rig:

1. Open any level with a `Pawn` or create one via **Blueprint > Pawn**.
2. Add a `Camera` component.
3. Tick **Use Pawn Control Rotation** on the camera (required — see [DisplayRigSetup.md](./DisplayRigSetup.md)).
4. Add a **DisplayXR Camera** component (under *DisplayXR* category) to the same pawn.
5. Adjust `IpdFactor`, `ParallaxFactor`, `ConvergenceDistance` if you want — defaults work.
6. Set this pawn as the level's **Default Pawn Class** via Game Mode.

For display-centric rigs (virtual display placed in the scene rather than on the camera), use a **DisplayXR Display** component instead. See [DisplayRigSetup.md](./DisplayRigSetup.md).

## 4. Press Play

Click Play in the editor. The current editor preview uses `SceneCapture2D` → a separate native 3D window (see [EditorPreview.md](./EditorPreview.md); native-XR preview is in-flight, see [EditorPreviewNative.md](./EditorPreviewNative.md)).

For the most faithful output, launch as a standalone game (**Play > Standalone Game** or build via `Scripts/PackagePlugin.bat`). In game mode the compositor path is fully active and the runtime drives the 3D display directly.

## 5. Verify the pipeline

In the Output Log look for:

```
DisplayXR: Session initialized
DisplayXR Session: Instance created, SystemId=...
DisplayXR Session: Display X.XXX x X.XXX m, XXXX x XXXX px
DisplayXR Session: Session created
DisplayXR: Tracking system created
```

If eye tracking is working you'll also see the 3D display switch to 3D mode and the image change as you move your head.

## Troubleshooting

- **`Failed to initialize session`** — runtime not installed or not the active OpenXR runtime. Re-verify step 1.
- **Game builds but renders in 2D** — `DisplayXRCore` HMD plugin priority didn't win. Check that `OpenXRHMD` isn't being forced active in `Engine/Config/DefaultEngine.ini`.
- **Everything loads but the 3D display stays in 2D mode** — call `RequestDisplayMode(true)` via Blueprint or ensure a `DisplayXR Camera` / `DisplayXR Display` component is in the scene and active.
- **Mac build fails with `OpenXRHMD module not found`** — you have a stale `.uplugin`. Current `DisplayXR.uplugin` depends only on `XRBase`, not `OpenXR`. Pull latest.

For packaging, rig tuning, and architecture background, see the [documentation index](./README.md).
