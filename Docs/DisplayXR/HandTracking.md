# Hand Tracking — DisplayXR UE Plugin

Ultraleap (Leap Motion) hands — or any other hand-tracking source the DisplayXR
runtime serves — reach Unreal through **standard OpenXR `XR_EXT_hand_tracking`**,
exposed as Unreal's **`IHandTracker`** modular feature. There is no vendor SDK
in the plugin and no vendor-specific API: the runtime owns the sensor (its
input-provider plug-in, runtime ADR-034), the plugin consumes 26 joints per hand
like any OpenXR app, and UE's existing hand nodes work unchanged.

Plugin version: **unreleased** (see `CHANGELOG.md`). Runtime: any DisplayXR
runtime that advertises `XR_EXT_hand_tracking` with a hand-tracking input
provider registered (the Ultraleap provider ships with the runtime; it needs the
Ultraleap tracking service running and a device attached).

## What you get

| Surface | How |
|---|---|
| Blueprint **Get Hand Tracking State** (`UHeadMountedDisplayFunctionLibrary::GetHandTrackingState`) | `FDisplayXRDevice::GetHandTrackingState` fills `FXRHandTrackingState` (26 `HandKeyLocations` / `HandKeyRotations` / `HandKeyRadii`, indexed by `EHandKeypoint`), `bValid`, `TrackingStatus`. Both `EXRSpaceType` values are honoured. |
| **XRVisualization** helpers (`UXRVisualizationFunctionLibrary::RenderHandTrackingState`-style nodes) | Consume the state above. |
| C++ `IHandTracker` | `IModularFeatures::Get().GetModularFeatureImplementations<IHandTracker>(IHandTracker::GetModularFeatureName())`, device type name **`"DisplayXR"`**. `GetKeypointState`, `GetAllKeypointStates`, `IsHandTrackingStateValid`. |

Not provided (see *Scope* below): motion-controller input (`IMotionController`
sources, `UMotionControllerComponent` driven by hand bones, Enhanced Input
actions, gestures, LiveLink).

## Pipeline

```
Compositor thread                 Game thread (FDisplayXRDevice::SetupViewFamily)
-----------------                 ---------------------------------------------
xrWaitFrame                       Session->Tick()
  -> predictedDisplayTime           -> LocateViews()        xrLocateViews(LOCAL, displayTime)
  -> SetPredictedDisplayTime()           + XrViewDisplayRawDXR on XrViewState::next
                                            -> displayPlanePose, canvasSizeMeters
                                     -> LocateHandJoints()  xrLocateHandJointsEXT x2 (LOCAL, same displayTime)
                                            -> joint_plane = inv(displayPlanePose) o joint_local
                                            -> FDisplayXRHandSample (double-buffered)

                                  Any UE hand consumer, same thread
                                    -> FDisplayXRHandTracking::RefreshCache()   (on generation change)
                                         display-plane metres -> rig-local UE units (rig mapping below)
                                    -> * GetTrackingToWorldTransform()          (the camera = the rig)
```

- **Enable.** `XR_EXT_hand_tracking` is probed before `xrCreateInstance` and
  only then requested (an unsupported extension fails instance creation). It is
  optional: the core-only retry drops it with the other optional extensions.
- **Support.** `XrSystemHandTrackingPropertiesEXT.supportsHandTracking` is read
  with the display info. The runtime reports `true` only when a hand-tracking
  input provider claimed the hand-tracking roles — a box with no Ultraleap
  registered advertises the extension and reports no support, and the plugin
  then serves nothing (no error).
- **Trackers.** Left/right `XrHandTrackerEXT` (default joint set) are created
  for every session the plugin creates and destroyed before that session.
- **Locate.** Both hands every frame, in the plugin's LOCAL reference space, at
  the predicted display time `xrWaitFrame` published. Time 0 (no frame yet) is
  skipped — it would be `XR_ERROR_TIME_INVALID`. The session's parked state
  between editor play sessions skips the locate too.
- **Hold-last-pose.** When `isActive` is false the previous joints are kept and
  flagged not tracked (Epic's model). `IsHandTrackingStateValid` is true while at
  least one hand is active.

## Spaces and the rig mapping

The joint poses the runtime returns are in the locate (LOCAL) space. Nothing in
the plugin assumes where LOCAL's origin is physically; instead the runtime's raw
rig channel (`XrViewDisplayRawDXR`, chained on `XrViewState::next` of the view
locate) says where the **physical display plane** sits in that space, and every
joint is rebased onto it:

```
joint_plane = inv(displayPlanePose) o joint_local
```

This is exactly what the runtime's reference app (`cube_handle_d3d11_win
--hands`, runtime #1370) does, and it is the hardware-validated convention. The
result is "display-plane space": OpenXR metres, origin at the panel centre, +X
right, +Y up, **+Z toward the viewer**. A hand 30 cm in front of the panel
centre is at `(0, 0, 0.3)`.

From there the hands follow the **same mapping the eyes already take**, so a
physical hand lands where the tracked eyes say it should relative to the scene:

| Rig | Camera means | Joint in rig space (metres) | Why |
|---|---|---|---|
| `UDisplayXRDisplay` (display-centric) | the virtual display plane | `m2v * joint_plane`, `m2v = virtualDisplayHeight / canvasHeight` | the runtime scaled the eyes by the same m2v (`XrDisplayRigDXR`); `PerspectiveFactor`, `IpdFactor`, `ParallaxFactor` are eye-only tunables and do not touch the hands |
| `UDisplayXRCamera` (camera-centric) | the nominal viewer | `joint_plane - (0, 0, nominalZ)` | the camera rig places the camera at the nominal viewer with `metersToVirtual = 1` (`dxr_view_math.c` camera3d: `eye_local = m2v * (eye - (0,0,nominalZ))`) |

Then OpenXR -> UE axes/units (`DisplayXRStereoMath.h`, Epic-equivalent
`ToFTransform`: position `(-z, x, y) * 100`, orientation `(-z, x, y, -w)`), and
finally **tracking -> world**, applied at query time:

- `FDisplayXRDevice::GetTrackingToWorldTransform()` returns the camera the local
  player renders from (the view point before the per-eye offset). That is the
  plugin's rig: `XrView.pose` comes back rig-local because the session submits an
  identity rig pose, and `CalculateStereoViewOffset` composes it onto the view
  rotation/location — hands compose the same way.
- Consequence (and the intent, runtime ADR-034 Amendment 2): WASD / mouse-look /
  any camera motion **carries the hands**; eye-tracked head parallax **does not**
  — your real hand stays where your real hand is, above the sensor on the desk.

`EXRSpaceType::XRTrackingSpace` returns the rig-local transforms (camera at the
origin, UE units); `UnrealWorldSpace` applies the camera transform.

## Diagnostics

Log category `LogDisplayXRSession`:

```
DisplayXR Session: XR_EXT_hand_tracking: AVAILABLE (IHandTracker will be served)
DisplayXR Session: XrSystemHandTrackingPropertiesEXT.supportsHandTracking = 1
DisplayXR Session: hand trackers created (left + right, default joint set, 26 joints each)
HandJoints #1: plane=1 L(active=1 wrist 0.120,-0.080,0.310) R(active=0 wrist ...) [display-plane m]
```

`supportsHandTracking = 0` with the trailing "(no hand-tracking input provider
registered or present ...)" means the runtime has no joint source: check
`displayxr-cli selftest` / `input list` on the box, the Ultraleap service, and
the USB connection. `plane=0` in the `HandJoints` line means the raw rig channel
was not filled (no `XR_DXR_view_rig`): joints are then kept in LOCAL as-is.

Category `LogDisplayXRHands` (Verbose) prints the rig mapping actually applied
(`rig=display scale=1.000 offsetZ=0.000` / `rig=camera scale=1.000
offsetZ=-0.600`).

## Validation (Windows box with an Ultraleap attached)

The plugin cannot be built on a Mac without Xcode and the runtime's hand source
is Windows hardware, so the acceptance run is:

1. Runtime: `displayxr-cli selftest` passes and the Ultraleap input provider is
   registered (`displayxr-cli input list`); `cube_handle_d3d11_win --hands` draws
   joint markers that follow your hands — this proves the runtime side before
   touching Unreal.
2. Build the plugin into a UE 5.7 project (`Plugins/DisplayXR`), open a map with
   a `UDisplayXRDisplay` rig on the player camera (the `DisplayRigSetup.md`
   pawn), press Play on the 3D display panel.
3. `Output Log`, filter `DisplayXRSession`: the three lines above, then
   `HandJoints #N` lines with `active=1` for a hand held over the sensor.
4. Drop a Blueprint tick that calls **Get Hand Tracking State** (Left, Unreal
   World Space) and draws a debug sphere at each `HandKeyLocations[i]` with
   radius `HandKeyRadii[i]` (or use the XRVisualization render nodes). Expected:
   26 spheres per hand, palm/wrist ~1–2 cm radius, fingertips ~0.5–1 cm, moving
   1:1 with your hand; a hand 30 cm in front of the panel centre sits ~30 UE
   units in front of the display plane (i.e. behind the camera on the display
   rig, between the viewer and the panel), left of centre for your left hand.
5. Turn the camera with the mouse: the spheres turn with it. Lean left/right
   (eye tracking): the spheres stay put relative to the panel.
6. `stat unit` / `stat game` while the hands are tracked: the locate costs two
   `xrLocateHandJointsEXT` calls per frame on the game thread; no measurable
   change expected. `stat fps` stays at the panel rate.
7. Hand out of view: the last pose holds, `TrackingStatus = NotTracked`; both
   hands out: `bValid = false`. Unplug the Ultraleap mid-session: same, no crash;
   replug: tracking resumes (the runtime re-arbitrates the roles, ADR-034
   Amendment 3).

## Scope

In: joints via `IHandTracker` / `GetHandTrackingState` / XRVisualization.

Out (follow-up issue in this repo, "Motion-controller input from hand tracking /
Ultraleap Tier 1 / gamepad"): `IMotionController` motion sources
(`LeftPalm`, `RightIndexTip`, ...) so `UMotionControllerComponent` can follow a
bone, pinch/grab as input actions, Enhanced Input mapping, LiveLink subjects,
and the runtime's action-system controllers (`XR_EXT_hand_interaction`,
Ultraleap Tier 1, qwerty/gamepad emulation).

## Files

- `Source/DisplayXRCore/Private/DisplayXRSession.{h,cpp}` — extension enable,
  system support, tracker lifetime, `LocateHandJoints`, raw rig channel.
- `Source/DisplayXRCore/Private/DisplayXRHandTracking.{h,cpp}` — the
  `IHandTracker`; rig mapping; tracking -> world.
- `Source/DisplayXRCore/Private/Rendering/DisplayXRDevice.{h,cpp}` —
  `GetTrackingToWorldTransform`, `GetHandTrackingState`.
- `Source/DisplayXRCore/Private/Native/dxr_pose_math.h` +
  `Scripts/tests/pose_math_test.cpp` — UE-free pose math and the standalone
  test the lint workflow runs (`clang++ -std=c++17 -I
  Source/DisplayXRCore/Private/Native Scripts/tests/pose_math_test.cpp`).
