# Controller and stylus input

The DisplayXR runtime serves controllers and 6-DoF styluses from **input-provider plug-ins** (runtime ADR-034), through standard OpenXR actions. Whatever provider supplies a device, an Unreal app sees it the same way: as Unreal **keys** (usable in Enhanced Input), **motion-controller sources** for `UMotionControllerComponent`, and **haptics**. Nothing in your project talks to OpenXR.

Two pieces do the work:

| Module | Role |
|---|---|
| `DisplayXRCore` | Owns the OpenXR session, so it also owns the action set: creates it, suggests bindings, attaches it to every session, syncs it each frame and locates the poses (`FDisplayXRControllers`, public API `FDisplayXRControllerAPI` in `DisplayXRControllerTypes.h`). |
| `DisplayXRInput` | Maps that state onto Unreal input: `FDisplayXRInputDevice` (`IInputDevice` + `IMotionController` "DisplayXR" + `IHapticDevice`), the `FDisplayXRKeys`, and the Blueprint library `UDisplayXRInputFunctionLibrary`. |

The display path is unchanged; the action set is independent of rendering.

## Interaction profiles

The plug-in suggests bindings for two profiles on `/user/hand/left` and `/user/hand/right`:

| Profile | When | Bound |
|---|---|---|
| `/interaction_profiles/khr/simple_controller` | always | grip pose, aim pose, select, menu, haptic |
| `/interaction_profiles/logitech/mx_ink_stylus_logitech` | only when the runtime advertises `XR_LOGITECH_mx_ink_stylus_interaction` (the instance then enables it) | grip, aim and tip poses, tip force, middle force, front and back cluster click and double tap, docked, system, haptic. *Select* is also bound to the tip (the runtime thresholds the force), so a generic "select" works with a controller and a stylus alike. |

The runtime picks the profile for each hand from what its provider supports. A runtime without the stylus extension still serves a stylus provider through `simple_controller` (poses, select, haptics; no forces or cluster buttons). If a runtime rejects some optional stylus component, the bindings are retried with fewer components (the log line names the tier) instead of losing the stylus.

`Output Log` filter `LogDisplayXRControllers` shows what was suggested, the profile bound to each hand when it changes, and a throttled pose line.

## Keys (Enhanced Input)

Menu category **DisplayXR**. Gamepad-class keys, player 0.

| Key (FName) | Type | Source |
|---|---|---|
| `DisplayXR_Left_Select` / `DisplayXR_Right_Select` | button | simple `/input/select/click`; stylus tip |
| `DisplayXR_Left_Menu` / `DisplayXR_Right_Menu` | button | simple `/input/menu/click` |
| `DisplayXR_Left_System` / `DisplayXR_Right_System` | button | stylus `/input/system/click` (the runtime may reserve it) |
| `DisplayXR_Left_Tip_Force` / `DisplayXR_Right_Tip_Force` | axis 1D, 0..1 | stylus `/input/tip_logitech/force` |
| `DisplayXR_Left_Middle_Force` / `DisplayXR_Right_Middle_Force` | axis 1D, 0..1 | stylus `/input/cluster_middle_logitech/force` |
| `DisplayXR_Left_Front_Click` / `DisplayXR_Right_Front_Click` | button | stylus `/input/cluster_front_logitech/click` |
| `DisplayXR_Left_Front_DoubleTap` / `DisplayXR_Right_Front_DoubleTap` | button | stylus `/input/cluster_front_logitech/double_tap_logitech` |
| `DisplayXR_Left_Back_Click` / `DisplayXR_Right_Back_Click` | button | stylus `/input/cluster_back_logitech/click` |
| `DisplayXR_Left_Back_DoubleTap` / `DisplayXR_Right_Back_DoubleTap` | button | stylus `/input/cluster_back_logitech/double_tap_logitech` |
| `DisplayXR_Left_Docked` / `DisplayXR_Right_Docked` | button | stylus `/input/dock_logitech/docked_logitech` |

From C++ the same keys are `FDisplayXRKeys::Left_Tip_Force` and so on (`DisplayXRInputKeys.h`).

Example: a drawing action. Create an Input Action `IA_Draw` (Value Type *Axis1D*), map `DisplayXR_Right_Tip_Force` to it in your Input Mapping Context, and read the value in the *Triggered* event as the stroke pressure. For press-style use, give the mapping a *Pressed* trigger with an actuation threshold.

## Motion sources (poses)

Add a `MotionController` component and set its **Motion Source**:

| Motion source | Pose |
|---|---|
| `Left` / `Right` | grip |
| `LeftGrip` / `RightGrip` | grip |
| `LeftAim` / `RightAim` | aim |
| `LeftTip` / `RightTip` | stylus tip; the aim pose on a profile without a tip |

**Attach the component under the camera the DisplayXR rig drives** (the camera with the `DisplayXRDisplay` / `DisplayXRCamera` component). DisplayXR's tracking space is the rig frame, with that camera at the origin (`FDisplayXRDevice::GetTrackingToWorldTransform`), exactly as for the hand joints. Moving the camera (WASD, mouse-look, a navigating provider) carries the stylus with it; eye-tracked parallax never moves it.

`IMotionController::GetCustomParameterValue` answers `TipForce` and `MiddleForce` for any of the sources above.

### Where the pen lands

A stylus provider publishes its poses relative to the physical display plane. The runtime places that plane where the rig the plug-in chained on `xrLocateViews` puts it, in metres (runtime `docs/specs/runtime/input-provider-discovery.md` §4b.4). The views are scaled by the rig, so the plug-in applies the same gain about that plane before handing the pose to Unreal:

- **Display rig** (`DisplayXRDisplay`): gain `PerspectiveFactor × VirtualDisplayHeight / canvas height`, about the display plane. A tip touching the glass sits at zero parallax; a tip 1 cm in front of the glass lands `gain` cm in front of the virtual display, where its rendered image appears.
- **Camera rig** (`DisplayXRCamera`): the glass is on the convergence plane (`1 / InvConvergenceDistance` in front of the camera), gain = the camera's virtual screen height there over the canvas height, as the runtime does for hand joints (`XR_DXR_view_rig` spec 5). At infinite convergence the pose stays metric.

`DisplayXR.Controllers.RigMapping 0` turns this off and hands Unreal the pose as located (LOCAL metres): use it for a stage-anchored device that is not display-relative, or to debug. The math is `dxr_pose::ControllerRigMapping` in `Native/dxr_pose_math.h`, checked by `Scripts/tests/pose_math_test.cpp`.

Poses are synced once per frame on the game thread, right after the view and hand-joint locates and at the same predicted display time, so a motion-controller component reads the previous frame's sync.

## Haptics

`Play Haptic Effect`, `Set Haptics By Value` and force feedback reach `xrApplyHapticFeedback` on the hand's `/output/haptic`. Frequency is left to the runtime (`XR_FREQUENCY_UNSPECIFIED`); an amplitude of 0 stops the vibration.

## Blueprint

`UDisplayXRInputFunctionLibrary`, category *DisplayXR | Input*:

- **Is DisplayXR Controller Input Available**: the action set is attached to a live session.
- **Get DisplayXR Controller Info** (Hand): `bActive`, `bIsStylus`, the bound `InteractionProfile` path.
- **Get DisplayXR Stylus Forces** (Hand): tip and middle force, 0..1.

## Validation

1. `displayxr-cli input list` (runtime) shows the provider and its devices.
2. Start the app and check `LogDisplayXRControllers`: `bindings suggested: simple_controller=yes, mx_ink_stylus=yes|no`, then `action set attached`, then `right hand interaction profile: /interaction_profiles/...` once the device is bound.
3. Add a `MotionController` (source `RightTip`) with a small sphere under the rig camera, press Play, and touch the glass: the sphere must sit on the pen tip at zero parallax.
4. Bind `DisplayXR_Right_Tip_Force` to an axis action and print it.

## Limits

- Player 0 only (one local player, one DisplayXR session).
- Velocities are not reported (`IMotionController` answers without them).
- `/input/system/click` may be reserved by the runtime and never reach the app.
