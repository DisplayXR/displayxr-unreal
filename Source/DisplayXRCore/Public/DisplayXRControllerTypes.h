// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

/**
 * Controllers and styluses from any DisplayXR input provider, as the core
 * module's OpenXR action set reads them each frame.
 *
 * The runtime serves controllers through standard OpenXR actions, whichever
 * input-provider plug-in supplies them. The core module creates one action
 * set, suggests bindings for /interaction_profiles/khr/simple_controller and
 * (when the runtime advertises XR_LOGITECH_mx_ink_stylus_interaction) the
 * MX Ink stylus profile, syncs it on the game thread right after the view
 * locate, and publishes the result here. The DisplayXRInput module turns it
 * into Unreal keys, motion-controller sources and haptics; nothing else needs
 * to touch OpenXR.
 *
 * Poses are in the plug-in's tracking space: the rig frame the views are in
 * (the camera the local player renders from at the origin), converted to UE
 * axes but still in METRES — multiply by the world-to-metres scale. Tracking
 * -> world is the transform FDisplayXRDevice::GetTrackingToWorldTransform()
 * publishes, as for the hand joints.
 */

/** One located pose (grip, aim or tip). */
struct FDisplayXRControllerPose
{
	/** UE axes (X forward, Y right, Z up), metres, tracking (rig) space. */
	FVector PositionMeters = FVector::ZeroVector;
	FQuat Orientation = FQuat::Identity;
	/** Position and orientation valid (XR_SPACE_LOCATION_*_VALID_BIT both set). */
	bool bValid = false;
	/** Actively tracked this frame (both *_TRACKED_BIT set). */
	bool bTracked = false;
};

/** One hand / subaction path (/user/hand/left or /user/hand/right). */
struct FDisplayXRControllerState
{
	/** Any binding of this hand is active, i.e. a device is bound to it. */
	bool bActive = false;
	/** The current interaction profile is the MX Ink stylus profile. */
	bool bIsStylus = false;
	/** xrGetCurrentInteractionProfile for this hand, empty when none. */
	FString InteractionProfile;

	FDisplayXRControllerPose Grip;
	FDisplayXRControllerPose Aim;
	/** Stylus tip (/input/tip_logitech/pose). Invalid on a profile without one. */
	FDisplayXRControllerPose Tip;

	// Generic controller (khr/simple_controller; also bound on the stylus).
	bool bSelect = false;  //!< simple: /input/select/click; stylus: the tip, thresholded by the runtime
	bool bMenu = false;    //!< simple: /input/menu/click

	// Stylus (MX Ink profile).
	float TipForce = 0.0f;     //!< /input/tip_logitech/force, 0..1
	float MiddleForce = 0.0f;  //!< /input/cluster_middle_logitech/force, 0..1
	bool bFrontClick = false;
	bool bFrontDoubleTap = false;
	bool bBackClick = false;
	bool bBackDoubleTap = false;
	bool bDocked = false;
	bool bSystemClick = false;
};

/** Both hands from one xrSyncActions pass. */
struct FDisplayXRControllerSnapshot
{
	FDisplayXRControllerState Hands[2]; // 0 = left, 1 = right
	/** Bumped on every sync; 0 = never synced. */
	uint64 Generation = 0;
};

/**
 * Entry points the DisplayXRInput module (or game code) uses. All return
 * false / do nothing while the DisplayXR session is not up.
 */
struct DISPLAYXRCORE_API FDisplayXRControllerAPI
{
	/** True when the action set is attached to a live session. */
	static bool IsAvailable();

	/** True when the instance enabled the MX Ink stylus interaction profile. */
	static bool IsStylusProfileEnabled();

	/** Latest published snapshot, copied under a lock. Thread-safe; callable
	 *  from the render thread (motion-controller late update). */
	static bool GetSnapshot(FDisplayXRControllerSnapshot& Out);

	/**
	 * xrApplyHapticFeedback on /output/haptic of one hand (0 = left, 1 = right).
	 * Amplitude 0..1; FrequencyHz 0 = runtime default; DurationSeconds <= 0 =
	 * the runtime's minimum pulse. Game thread.
	 */
	static bool ApplyHaptic(int32 HandIndex, float Amplitude, float FrequencyHz, float DurationSeconds);

	/** xrStopHapticFeedback on one hand. Game thread. */
	static void StopHaptic(int32 HandIndex);
};
