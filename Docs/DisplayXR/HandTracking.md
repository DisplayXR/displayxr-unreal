# Hand Tracking — DisplayXR UE Plugin

Hands from a hand-tracking camera — or any other hand-tracking source the
DisplayXR runtime serves — reach Unreal through **standard OpenXR `XR_EXT_hand_tracking`**,
exposed as Unreal's **`IHandTracker`** modular feature. There is no vendor SDK
in the plugin and no vendor-specific API: the runtime owns the sensor (its
input-provider plug-in, runtime ADR-034), the plugin consumes 26 joints per hand
like any OpenXR app, and UE's existing hand nodes work unchanged.

Plugin version: **unreleased** (see `CHANGELOG.md`). Runtime: any DisplayXR
runtime that advertises `XR_EXT_hand_tracking` with a hand-tracking input
provider registered (the hand-tracking camera's provider ships with the runtime;
it needs the camera's tracking service running and a device attached).

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
                                            runtime view_rig spec >= 4: the view locate's rig chained
                                              on XrHandJointsLocateInfoEXT::next -> joints in the RIG frame
                                            older runtime: joint_plane = inv(displayPlanePose) o joint_local
                                            -> FDisplayXRHandSample (double-buffered, bRigFrame)

                                  Any UE hand consumer, same thread
                                    -> FDisplayXRHandTracking::RefreshCache()   (on generation change)
                                         rig frame: axes/units only; display plane: rig mapping below
                                    -> * GetTrackingToWorldTransform()          (the camera = the rig)
```

- **Enable.** `XR_EXT_hand_tracking` is probed before `xrCreateInstance` and
  only then requested (an unsupported extension fails instance creation). It is
  optional: the core-only retry drops it with the other optional extensions.
- **Support.** `XrSystemHandTrackingPropertiesEXT.supportsHandTracking` is read
  with the display info. The runtime reports `true` only when a hand-tracking
  input provider claimed the hand-tracking roles — a box with no hand-tracking camera
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

The hands have to land in the frame the **views** are in. The plugin chains a
`XrDisplayRigDXR` / `XrCameraRigDXR` on every view locate, so the views are in
the rig frame: centred on the **canvas** (the window, or the editor viewport
the zone-scoped locate frames), scaled like the eyes, at the rig pose (identity,
so rig-local).

**Runtime with `XR_DXR_view_rig` spec >= 4 (runtime #2189, v2.47.1+): rig-frame
joints.** The session chains the same rig descriptor the view locate just used on
`XrHandJointsLocateInfoEXT::next`, and the runtime returns every joint in that
rig frame, with the transform its views get (display rig: minus the canvas
centre, x `perspectiveFactor * virtualDisplayHeight / canvasHeight`; camera rig:
x `metersToVirtual` about the nominal viewer plane; radii scaled alike). The
plugin applies nothing but axes/units. This is the only path that is right when
the canvas is not centred on the display: the canvas is the runtime's, and the
plugin does not know where it sits. Gated on the advertised `extensionVersion`
(not the name): an older runtime ignores the chained rig and answers in base
space without an error. Views are always located before joints in a frame, as
the spec requires (the runtime takes the canvas from the latest view locate).

**Older runtime (fallback): display-plane joints.** Joints are located in LOCAL
and rebased onto the plane the raw rig channel (`XrViewDisplayRawDXR`) reports:

```
joint_plane = inv(displayPlanePose) o joint_local
```

then the eyes' convention is re-derived:

| Rig | Camera means | Joint in rig space (metres) |
|---|---|---|
| `UDisplayXRDisplay` (display-centric) | the virtual display plane | `m2v * joint_plane`, `m2v = virtualDisplayHeight / canvasHeight` |
| `UDisplayXRCamera` (camera-centric) | the nominal viewer | `joint_plane - (0, 0, nominalZ)` (the session passes `metersToVirtual = 1`) |

This ignores where the canvas sits on the display, so it is right only for a
canvas centred on it (a fullscreen game). On a runtime that maps
display-plane-relative hand sources onto a chained rig's plane (v2.56.0+, which
also has spec 4, so this fallback never runs there) it would be badly wrong: once
a view locate chains a rig, `displayPlanePose` is the rig pose and such a joint
lands at the rig's image of the physical display centre, `-m2v * canvasCentre`,
an offset in rig units under a hand in metres. A window 3.5 cm off centre at
m2v ~15 put the hands about 0.5 m off-screen that way. The fallback mapping
lives in `dxr_pose::HandRigMappingFor`; `Scripts/tests/pose_math_test.cpp`
models that case.

`IpdFactor` and `ParallaxFactor` are eye-pair tunables and never touch the hands.

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
DisplayXR Session: rig-scoped hand joints: YES (the rig is chained on xrLocateHandJointsEXT)
HandJoints #1: frame=rig L(active=1 wrist 1.237,-0.825,3.196) R(active=0 wrist ...) [rig-local m, canvas-centred]
```

`supportsHandTracking = 0` with the trailing "(no hand-tracking input provider
registered or present ...)" means the runtime has no joint source: check
`displayxr-cli selftest` / `input list` on the box, the hand-tracking camera's
service, and the USB connection. `frame=` in the `HandJoints` line says which frame the
joints are in: `rig` (runtime-mapped, spec 4), `plane` (display-plane fallback)
or `local` (no raw rig channel, no `XR_DXR_view_rig`: joints kept in LOCAL
as-is). `rig-scoped hand joints: NO` means the runtime is older than
`XR_DXR_view_rig` spec 4; hands are then right only on a centred canvas.

Category `LogDisplayXRHands` (Verbose) prints the mapping actually applied
(`frame=rig (runtime-mapped) scale=1.000 offsetZ=0.000`, or on the fallback
`rig=display ... scale=14.9` / `rig=camera ... offsetZ=-0.600`).

## Validation (Windows box with a hand-tracking camera attached)

The plugin cannot be built on a Mac without Xcode and the runtime's hand source
is Windows hardware, so the acceptance run is:

1. Runtime: `displayxr-cli selftest` passes and the hand-tracking camera's input
   provider is registered (`displayxr-cli input list`); `cube_handle_d3d11_win --hands` draws
   joint markers that follow your hands — this proves the runtime side before
   touching Unreal.
2. Build the plugin into a UE 5.7 project (`Plugins/DisplayXR`), open a map with
   a `UDisplayXRDisplay` rig on the player camera (the `DisplayRigSetup.md`
   pawn), press Play on the 3D display panel.
3. `Output Log`, filter `DisplayXRSession`: the three lines above, then
   `HandJoints #N` lines with `active=1` for a hand held over the sensor.
4. Type `DisplayXR.Hands.Debug 1` in the in-game console to draw every
   tracked joint, both hands, as a sphere at its radius: left hand cyan, right
   hand orange, grey while a hand holds its last pose. `0` turns it off. It is
   compiled out of Shipping builds, like all debug drawing. Expected:
   26 spheres per hand, palm/wrist ~1–2 cm radius, fingertips ~0.5–1 cm, moving
   1:1 with your hand; a hand 30 cm in front of the panel centre sits ~30 UE
   units in front of the display plane (i.e. behind the camera on the display
   rig, between the viewer and the panel), left of centre for your left hand.
   To draw your own markers instead (a Blueprint tick that calls **Get Hand
   Tracking State** and draws at each `HandKeyLocations[i]` with radius
   `HandKeyRadii[i]`, or the XRVisualization render nodes), use depth priority
   **Foreground** for debug shapes: set the Blueprint **Draw Debug Sphere**
   node's *Depth Priority* pin to `Foreground` (its default is `World`), or pass
   `SDPG_Foreground` as `DrawDebugSphere`'s `DepthPriority` in C++. `SDPG_World`
   debug draws are depth-tested against the temporally upscaled scene depth, and
   while temporal AA (TAA/TSR) is on they are lost in the **second view**: they
   appear in the left view only (#70). For markers that must depth-sort with the scene,
   use mesh components (e.g. instanced spheres), not debug draw.
5. Turn the camera with the mouse: the spheres turn with it. Lean left/right
   (eye tracking): the spheres stay put relative to the panel.
6. `stat unit` / `stat game` while the hands are tracked: the locate costs two
   `xrLocateHandJointsEXT` calls per frame on the game thread; no measurable
   change expected. `stat fps` stays at the panel rate.
7. Hand out of view: the last pose holds, `TrackingStatus = NotTracked`; both
   hands out: `bValid = false`. Unplug the hand-tracking camera mid-session:
   same, no crash; replug: tracking resumes (the runtime re-arbitrates the roles, ADR-034
   Amendment 3).

## Scope

In: joints via `IHandTracker` / `GetHandTrackingState` / XRVisualization.

Out (follow-up issue in this repo, "Motion-controller input from hand tracking /
hand-tracking camera controllers / gamepad"): `IMotionController` motion sources
(`LeftPalm`, `RightIndexTip`, ...) so `UMotionControllerComponent` can follow a
bone, pinch/grab as input actions, Enhanced Input mapping, LiveLink subjects,
and the runtime's action-system controllers (`XR_EXT_hand_interaction`,
the hand-tracking camera's controller emulation, qwerty/gamepad emulation).

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
