// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "IHandTracker.h"
#include "HeadMountedDisplayTypes.h"
#include "InputCoreTypes.h"
#include "Engine/EngineBaseTypes.h"  // ELevelTick (debug-view tick delegate)
#include "EngineDefines.h"           // UE_ENABLE_DEBUG_DRAWING (gates members below; not in every PCH)
#include "DisplayXRSession.h"

class UWorld;

/**
 * Unreal's IHandTracker modular feature, served from the session's
 * XR_EXT_hand_tracking joints.
 *
 * Registered under IHandTracker::GetModularFeatureName() for the module's
 * lifetime, device type name "DisplayXR". UE's own hand nodes work unchanged:
 * UHeadMountedDisplayFunctionLibrary::GetHandTrackingState (Blueprint) reaches
 * FDisplayXRDevice::GetHandTrackingState, which calls back in here, and the
 * XRVisualization helpers consume that state.
 *
 * Spaces. This class maps the session's cached joints into UE tracking space —
 * the plug-in's rig space, i.e. the camera the local player renders from.
 *
 *  - Runtime with XR_DXR_view_rig spec >= 4 (#2189): the session chains the
 *    view locate's rig on xrLocateHandJointsEXT and the runtime returns the
 *    joints in the rig frame, exactly the frame the views are in. That frame
 *    is centred on the CANVAS (the window / editor viewport the views frame),
 *    not on the display, and is scaled like the eyes (display rig:
 *    perspectiveFactor * virtualDisplayHeight / canvasHeight; camera rig:
 *    metersToVirtual about the nominal viewer plane). Only axes/units are
 *    left to apply here. This is the path that is right for an off-centre
 *    canvas.
 *  - Older runtime (fallback): joints are in DISPLAY-PLANE space (OpenXR
 *    metres, origin at the physical display centre, +Z toward the viewer) and
 *    the eyes' convention is re-derived here: display rig scales by
 *    m2v = virtualDisplayHeight / canvasHeight; camera rig offsets by
 *    -nominalZ, unscaled (the session passes metersToVirtual = 1). This
 *    ignores where the canvas sits on the display, so hands are right only on
 *    a canvas centred on it.
 *
 * Either way IpdFactor / ParallaxFactor are eye-pair tunables and do not touch
 * the hands. The mapping choice is dxr_pose::HandRigMappingFor
 * (Native/dxr_pose_math.h, checked by Scripts/tests/pose_math_test.cpp).
 *
 * Tracking -> world is the camera transform FDisplayXRDevice publishes via
 * GetTrackingToWorldTransform(), applied at query time like Epic's
 * FOpenXRHandTracking does, so a hand physically in front of the panel lands
 * in front of the in-game camera and follows it (voluntary rig motion
 * carries the hands; eye-tracked parallax never does — runtime ADR-034
 * Amendment 2).
 *
 * Game thread only (the engine's own hand consumers are). The session's
 * buffers are thread-safe, the derived cache here is not.
 *
 * Debug view: `DisplayXR.Hands.Debug 1` draws every joint of both hands as a
 * sphere at its radius, from the world's post-actor tick, at SDPG_Foreground.
 * Foreground matters: SDPG_World debug lines are depth-tested against scene
 * depth, and under TAA/TSR the second view loses them (#70, cause open). Compiled out where debug drawing is (UE_ENABLE_DEBUG_DRAWING = 0).
 */
class FDisplayXRHandTracking : public IHandTracker
{
public:
	explicit FDisplayXRHandTracking(FDisplayXRSession* InSession);
	virtual ~FDisplayXRHandTracking();

	/** The name UE's device-type lookups match on ("DisplayXR"). */
	static FName DeviceTypeName();

	// --- IHandTracker ---
	virtual FName GetHandTrackerDeviceTypeName() const override;
	/** True while the session serves joints AND at least one hand is located
	 *  active this frame — "available and tracking", per the interface doc.
	 *  A hand that just left the sensor's view keeps returning its last pose
	 *  from GetAllKeypointStates with OutIsTracked = false (no pop to the
	 *  origin), as Epic's implementation does. */
	virtual bool IsHandTrackingStateValid() const override;
	virtual bool GetKeypointState(EControllerHand Hand, EHandKeypoint Keypoint,
		FTransform& OutTransform, float& OutRadius) const override;
	virtual bool GetAllKeypointStates(EControllerHand Hand, TArray<FVector>& OutPositions,
		TArray<FQuat>& OutRotations, TArray<float>& OutRadii, bool& OutIsTracked) const override;

	/** GetAllKeypointStates with an explicit space: UnrealWorldSpace applies
	 *  the tracking-to-world transform, XRTrackingSpace returns rig-local (UE
	 *  units, camera at the origin). Used by FDisplayXRDevice::GetHandTrackingState. */
	bool GetAllKeypointStatesInSpace(EControllerHand Hand, EXRSpaceType SpaceType,
		TArray<FVector>& OutPositions, TArray<FQuat>& OutRotations, TArray<float>& OutRadii,
		bool& OutIsTracked) const;

private:
	struct FHandCache
	{
		FTransform Keypoints[EHandKeypointCount];   // UE tracking (rig) space, UE units
		float Radii[EHandKeypointCount] = {};       // UE units
		bool bTracked = false;                      // located active this frame
		bool bEverTracked = false;                  // keypoints hold a real pose
	};

	/** Re-derive the UE-space caches when the session published a new locate. */
	void RefreshCache() const;
	static int32 HandIndex(EControllerHand Hand);
	static FTransform TrackingToWorld();

#if UE_ENABLE_DEBUG_DRAWING
	/** FWorldDelegates::OnWorldPostActorTick: draw the joints when DisplayXR.Hands.Debug is on. */
	void DrawDebugJoints(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	FDelegateHandle PostActorTickHandle;
#endif

	FDisplayXRSession* Session = nullptr;
	mutable uint64 CachedGeneration = ~0ull;
	mutable FHandCache Hands[2];
};
