# macOS Setup Guide

## Prerequisites

- UE 5.7+ installed via Epic Games Launcher
- Xcode (for C++ plugin compilation)
- DisplayXR runtime installed (either from package or built from source)

## Platform Architecture

Unreal's OpenXR plugin (`OpenXRHMD`) **does not ship on macOS**. Therefore, the DisplayXR plugin uses a different code path on Mac:

| | Windows/Android | macOS |
|---|---|---|
| **OpenXR integration** | `IOpenXRExtensionPlugin` hooks into Unreal's `FOpenXRHMD` | `FDisplayXRDirectSession` loads OpenXR runtime directly via `dlopen` |
| **Stereo injection** | `InsertOpenXRAPILayer()` hooks `xrLocateViews` | `FDisplayXRSceneViewExtension : FSceneViewExtensionBase` overrides camera |
| **Runtime discovery** | Unreal's OpenXR loader handles it | Plugin loads `libopenxr_displayxr.so` directly |
| **Compile flag** | `DISPLAYXR_USE_UNREAL_OPENXR=1` | `DISPLAYXR_USE_UNREAL_OPENXR=0` |

The Kooima math, components, rig manager, Blueprint API, and materials module are **identical** across platforms. Only the rendering injection point differs.

## Step 1: Set DisplayXR as Active OpenXR Runtime

The system active runtime file currently points to SRMonado. Switch it to DisplayXR:

```bash
# Check current active runtime
cat /etc/xdg/openxr/1/active_runtime.json

# Option A: Point to installed DisplayXR runtime
sudo cp /usr/local/share/openxr/1/openxr_displayxr.json /etc/xdg/openxr/1/active_runtime.json

# Option B: Point to development build
sudo tee /etc/xdg/openxr/1/active_runtime.json << 'EOF'
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR",
        "library_path": "/usr/local/lib/libopenxr_displayxr.so"
    }
}
EOF

# Option C: Use environment variable (per-process, no sudo needed)
export XR_RUNTIME_JSON=/usr/local/share/openxr/1/openxr_displayxr.json
```

Verify the runtime library exists:
```bash
ls -la /usr/local/lib/libopenxr_displayxr.so
```

## Step 2: Create a UE C++ Project

1. Open Unreal Editor 5.7
2. Create a new project: **Games > Blank > C++** (not Blueprint)
3. Name it (e.g., "DisplayXRTest")
4. Wait for project creation to complete
5. Close the editor

## Step 3: Install the Plugin

```bash
cd /path/to/DisplayXRTest/Plugins
mkdir DisplayXR

# Copy plugin files from the DisplayXR repo
cp /path/to/displayxr-unreal/DisplayXR.uplugin DisplayXR/
cp -r /path/to/displayxr-unreal/Source/DisplayXRCore DisplayXR/Source/
cp -r /path/to/displayxr-unreal/Source/DisplayXRMaterials DisplayXR/Source/
cp -r /path/to/displayxr-unreal/Source/DisplayXREditor DisplayXR/Source/
```

## Step 4: Disable the OpenXR Plugin Dependency (Mac Only)

Edit `DisplayXR/DisplayXR.uplugin` and remove or disable the OpenXR plugin dependency on Mac:

The plugin descriptor has `"Plugins": [{"Name": "OpenXR", "Enabled": true}]`. On Mac, this will fail because the OpenXR plugin doesn't exist. Either:

**Option A:** Remove the OpenXR dependency (safest for Mac-only testing):
```json
"Plugins": []
```

**Option B:** Make it platform-conditional (requires testing).

## Step 5: Build and Launch

Double-click the `.uproject` file. Unreal will detect the plugin and compile it via Xcode.

Check Output Log for:
```
DisplayXR: Core module started (direct OpenXR session path)
DisplayXR Direct: Loaded OpenXR from /usr/local/lib/libopenxr_displayxr.so
DisplayXR Direct: Instance created, SystemId=...
DisplayXR Direct: Display X.XXX x X.XXX m, XXXX x XXXX px
DisplayXR Direct: Session created
DisplayXR Direct: Session running
```

## Step 6: Test

1. Add a Camera Actor to the scene
2. Add a `DisplayXR Camera` component to it (from the DisplayXR category)
3. Adjust IpdFactor, ParallaxFactor, InvConvergenceDistance
4. The editor viewport should show Kooima-adjusted perspective

## How the Mac Path Works

```
Engine Startup:
  FDisplayXRCoreModule::StartupModule()
    → Creates FDisplayXRDirectSession
    → dlopen("libopenxr_displayxr.so")
    → xrCreateInstance() with XR_EXT_display_info + XR_EXT_cocoa_window_binding
    → xrGetSystemProperties() → queries display info
    → xrCreateSession() with Cocoa binding
    → Registers FDisplayXRSceneViewExtension

Each Frame:
  FDisplayXRDirectSession::Tick()
    → xrPollEvent (handle session state changes)
    → xrLocateViews (get raw eye positions)
    → camera3d/display3d_compute_views (Kooima projection)
    → Store stereo matrices in double buffer

  FDisplayXRSceneViewExtension::SetupViewPoint()
    → Read eye positions from direct session
    → Apply center-eye offset to editor camera

  FDisplayXRSceneViewExtension::SetupView()
    → Read Kooima projection matrices from direct session
    → Override camera projection matrix

  UDisplayXRCamera/Display::TickComponent()
    → Push tunables via FDisplayXRPlatform (routes to direct session)
```

## Troubleshooting

**"Failed to load OpenXR loader"**: The runtime library wasn't found. Check that `/usr/local/lib/libopenxr_displayxr.so` exists. If using a dev build, set `XR_RUNTIME_JSON` environment variable before launching UE.

**"xrCreateInstance failed"**: The runtime loaded but couldn't initialize. Check that the 3D display is connected and its driver is running.

**"xrCreateSession failed"**: Instance created but session failed. This can happen if no display is connected — the plugin will still provide display info but won't track eyes.

**Build error: "OpenXRHMD module not found"**: The `.uplugin` still has the OpenXR dependency. Remove it for Mac builds (see Step 4).
