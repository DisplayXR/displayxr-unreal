# Display Rig Setup

How to set up a pawn/camera rig for DisplayXR stereoscopic 3D in Unreal Engine.

## Quick Setup

1. Create a **Pawn** or **Character** Blueprint (e.g., `BP_DisplayRig`)
2. Add a **Camera Component** as the root or child of the root
3. **Enable `Use Pawn Control Rotation`** on the camera component — this is critical for mouse rotation to work
4. Optionally add a `DisplayXRDisplay` or `DisplayXRCamera` component to control stereo tunables
5. Set your **Game Mode** to use this pawn as the Default Pawn Class

## Why Use Pawn Control Rotation

UE's stereo rendering pipeline reads the camera's `ViewInfo.Rotation` to build the view matrix. If the camera component doesn't inherit the controller's rotation (`bUsePawnControlRotation = false`), the view rotation stays at (0,0,0) regardless of mouse input.

**Symptom without it**: WASD keys move the camera and mouse drag changes WASD direction, but the rendered view never rotates.

**What happens under the hood**:
- Mouse input → `APlayerController::AddControllerYawInput/PitchInput` → controller rotation updates
- WASD movement reads `GetControlRotation()` for forward/right vectors → movement direction changes ✓
- Camera component with `bUsePawnControlRotation = false` → `ViewInfo.Rotation = (0,0,0)` always ✗
- Camera component with `bUsePawnControlRotation = true` → `ViewInfo.Rotation = controller rotation` ✓
- UE's `LocalPlayer::CalcSceneView` builds the view matrix from `ViewInfo.Rotation`

## Display-Centric vs Camera-Centric

The stereo mode depends on which component is on the rig:

| Component | Mode | Use case |
|-----------|------|----------|
| `DisplayXRDisplay` | Display-centric (`bCameraCentric=false`) | Fixed display, viewer moves around it |
| `DisplayXRCamera` | Camera-centric (`bCameraCentric=true`) | FPS-style, camera moves through scene |
| Neither | Display-centric (default) | Tunables use defaults |

Both modes require `Use Pawn Control Rotation = true` on the camera for mouse rotation to work.

## Minimal Example (no custom component)

If you don't add a `DisplayXRDisplay` or `DisplayXRCamera` component, the plugin uses default tunables (display-centric, all factors = 1.0). This is sufficient for basic testing:

1. Create a Pawn Blueprint
2. Add a Camera Component with `Use Pawn Control Rotation = true`
3. Add a Floating Pawn Movement component (for WASD)
4. Set as Default Pawn in your Game Mode
5. Run — you get 3D output with parallax and mouse rotation

## Input Setup

The default pawn needs input bindings for mouse look and WASD movement. If using Enhanced Input, ensure your Input Mapping Context includes:
- **Mouse X/Y** → `AddControllerYawInput` / `AddControllerPitchInput`
- **WASD** → movement via `AddMovementInput`

The default `ADefaultPawn` class has these built-in. Custom pawns need to set them up explicitly.
