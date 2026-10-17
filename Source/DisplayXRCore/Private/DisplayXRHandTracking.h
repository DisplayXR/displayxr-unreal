// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "IHandTracker.h"
#include "HeadMountedDisplayTypes.h"
#include "InputCoreTypes.h"
#include "DisplayXRSession.h"

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
 * Spaces. The session caches joints in DISPLAY-PLANE space (OpenXR metres,
 * origin at the physical display centre, +Z toward the viewer). This class
 * maps them into UE tracking space — the plug-in's rig space, i.e. the camera
 * the local player renders from, with the SAME convention the eyes use:
 *
 *  - display rig (UDisplayXRDisplay): the camera IS the virtual display plane,
 *    so a joint is scaled by m2v = virtualDisplayHeight / canvasHeight (the
 *    scale the runtime applied to the eyes) and the plane sits at the origin.
 *    PerspectiveFactor / IpdFactor / ParallaxFactor are eye-only tunables and
 *    do not touch the hands.
 *  - camera rig (UDisplayXRCamera): the camera sits at the nominal viewer
 *    position (0, 0, nominalZ) in display space with metersToVirtual = 1 (what
 *    the session passes), so a joint is simply offset by -nominalZ along the
 *    display normal, unscaled.
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

	FDisplayXRSession* Session = nullptr;
	mutable uint64 CachedGeneration = ~0ull;
	mutable FHandCache Hands[2];
};
