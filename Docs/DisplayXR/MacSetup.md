# macOS Setup

Mac runs the same [Architecture](./Architecture.md) as Windows — one unified `FDisplayXRSession` loaded via `dlopen`. This doc covers the Mac-specific quirks the QuickStart doesn't handle.

For the generic install-and-go flow, start with [QuickStart.md](./QuickStart.md) and come back here for the Mac-specific bits.

## Prerequisites

- Unreal Engine 5.3+ installed via Epic Games Launcher
- Xcode (for C++ plugin compilation)
- DisplayXR runtime installed (either from a package or built from source — see [openxr-3d-display](https://github.com/dfattal/openxr-3d-display))

## Mac-specific quirk 1: no UE OpenXR plugin on Mac

Unreal's `OpenXR` plugin **does not ship on macOS**. Builds that depend on it will fail with `OpenXRHMD module not found`.

This is not a problem for DisplayXR directly — the current `DisplayXR.uplugin` only depends on `XRBase`, which is available on Mac. **If you see a `.uplugin` anywhere that lists `OpenXR` as a plugin dependency, it's stale — delete the entry on Mac.**

## Mac-specific quirk 2: set DisplayXR as the active OpenXR runtime

Unlike Windows (where the registry entry is usually set by the runtime installer), Mac's active-runtime manifest lives in a plain JSON file. You may need to point it at DisplayXR:

```bash
# See what's currently active
cat /etc/xdg/openxr/1/active_runtime.json

# Option A — point to an installed DisplayXR runtime
sudo cp /usr/local/share/openxr/1/openxr_displayxr.json /etc/xdg/openxr/1/active_runtime.json

# Option B — write it inline (useful for a dev build)
sudo tee /etc/xdg/openxr/1/active_runtime.json << 'EOF'
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR",
        "library_path": "/usr/local/lib/libopenxr_displayxr.dylib"
    }
}
EOF

# Option C — per-process override, no sudo needed
export XR_RUNTIME_JSON=/usr/local/share/openxr/1/openxr_displayxr.json
```

Verify the runtime library is readable:

```bash
ls -la /usr/local/lib/libopenxr_displayxr.dylib
```

## Mac-specific quirk 3: graphics binding

On Windows the session uses `XrGraphicsBindingD3D12KHR`. On Mac it uses `XR_EXT_cocoa_window_binding` via Metal. The session handles this internally via `#if PLATFORM_MAC` — you don't set any compile flag. If you're touching session code and need the Mac path, look for `PLATFORM_MAC || PLATFORM_LINUX` branches in `DisplayXRSession.cpp`.

## Install + build

Once the quirks above are sorted, install the plugin like on any platform:

1. Create a UE 5.3+ **C++** project (Blueprint-only can't compile plugins).
2. Close the editor.
3. Clone the repo into `Plugins/DisplayXR/`:
   ```bash
   cd <YourProject>
   mkdir -p Plugins
   git clone https://github.com/DisplayXR/displayxr-unreal.git Plugins/DisplayXR
   ```
4. Regenerate the Xcode project and build, or double-click the `.uproject` and let UE compile it.

Then see [QuickStart.md](./QuickStart.md) §3–5 for rig setup and verification.

## Troubleshooting

- **`Failed to load OpenXR loader`** — runtime library wasn't found. Check the `library_path` in `/etc/xdg/openxr/1/active_runtime.json` against what's actually on disk, or set `XR_RUNTIME_JSON` before launching UE.
- **`xrCreateInstance failed`** — runtime loaded but couldn't initialize. Check the 3D display is connected and its driver is running.
- **`xrCreateSession failed`** — instance created but session failed. This can happen if no display is connected — the plugin still provides display info but won't track eyes.
- **Build error: `OpenXRHMD module not found`** — the `.uplugin` still has an `OpenXR` dependency (see quirk 1 above). Pull latest or remove the entry manually.
- **`Session initialized` but rendering is 2D** — the HMD plugin priority bump didn't win. Ensure `DisplayXRCore` is actually loading (`PostConfigInit`). On Mac the compositor path is still being validated (tracked in [TODO.md](./TODO.md) §5); Mac may render in 2D by design until that lands.

## Status

Mac validation (end-to-end session + compositor) is tracked in [TODO.md](./TODO.md) §5 under "Platform coverage". The architecture works on Mac in principle; the last-mile testing is pending.
