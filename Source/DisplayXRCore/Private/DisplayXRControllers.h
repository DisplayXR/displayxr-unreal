// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "DisplayXRControllerTypes.h"

#include <openxr/openxr.h>

#include "Native/dxr_pose_math.h"

/** The vendored openxr.h defines it; the fallback keeps an older header building. */
#ifdef XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME
#define DISPLAYXR_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME
#else
#define DISPLAYXR_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME "XR_LOGITECH_mx_ink_stylus_interaction"
#endif

/**
 * The OpenXR action plumbing behind FDisplayXRControllerAPI: one action set
 * that surfaces the controllers and styluses of ANY DisplayXR input provider.
 *
 * Owned by FDisplayXRSession, which calls the lifecycle hooks in OpenXR's
 * order: action set + actions + suggested bindings once per instance (they
 * become immutable at the first attach), attach + action spaces once per
 * session (a PIE rebind recreates the session; the same action set attaches
 * to the new one), spaces destroyed before the session, the set before the
 * instance.
 *
 * Bindings suggested:
 *  - /interaction_profiles/khr/simple_controller — grip, aim, select, menu,
 *    haptic. Every runtime-side controller profile can fall back to it.
 *  - /interaction_profiles/logitech/mx_ink_stylus_logitech — only when the
 *    runtime advertised XR_LOGITECH_mx_ink_stylus_interaction and the instance
 *    enabled it (suggesting a profile of a disabled extension fails with
 *    XR_ERROR_PATH_UNSUPPORTED). Tip pose, tip / middle force, front / back
 *    cluster click + double tap, docked, system, haptic; select is also bound
 *    to the tip so a generic "select" works with either device.
 *
 * Threads: lifecycle + SyncAndLocate + haptics on the game thread (the same
 * thread that locates views and hand joints); GetSnapshot anywhere.
 */
class FDisplayXRControllers
{
public:
	FDisplayXRControllers() = default;
	~FDisplayXRControllers() = default;

	/** Create the action set, its actions and suggest bindings. Call once,
	 *  right after xrCreateInstance succeeded. */
	void OnInstanceCreated(XrInstance InInstance, PFN_xrGetInstanceProcAddr InGetProcAddr, bool bInStylusProfileEnabled);

	/** Attach the action set to a new session and create its action spaces. */
	void OnSessionCreated(XrSession InSession);

	/** Destroy the action spaces. MUST run before xrDestroySession. */
	void OnSessionDestroying();

	/** Destroy the action set (and with it the actions). Before xrDestroyInstance. */
	void OnInstanceDestroying();

	/** XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED seen (any thread). */
	void NotifyInteractionProfileChanged() { bProfileDirty.Store(true); }

	/**
	 * xrSyncActions + read every action + locate the poses in BaseSpace at Time
	 * (the view locate's space and display time), map them into the rig frame
	 * with Mapping and publish the snapshot. Game thread, after LocateViews.
	 */
	void SyncAndLocate(XrSpace BaseSpace, XrTime Time, const dxr_pose::ControllerRigMapping& Mapping);

	bool IsAttached() const { return bAttached.Load(); }
	bool IsStylusProfileEnabled() const { return bStylusProfileEnabled; }

	void GetSnapshot(FDisplayXRControllerSnapshot& Out) const;

	bool ApplyHaptic(int32 HandIndex, float Amplitude, float FrequencyHz, float DurationSeconds);
	void StopHaptic(int32 HandIndex);

private:
	enum EPoseAction : int32 { Pose_Grip = 0, Pose_Aim, Pose_Tip, Pose_Count };

	XrAction CreateAction(const char* Name, const char* LocalizedName, XrActionType Type);
	XrPath Path(const char* Str) const;
	XrPath HandComponentPath(const char* Hand, const char* Component) const;
	bool SuggestSimpleController();
	bool SuggestStylus();
	bool Suggest(const char* Profile, const TArray<XrActionSuggestedBinding>& Bindings);
	void RefreshInteractionProfiles(FDisplayXRControllerSnapshot& Snap);
	void DestroySpaces();

	XrInstance Instance = XR_NULL_HANDLE;
	XrSession Session = XR_NULL_HANDLE;
	PFN_xrGetInstanceProcAddr GetProcAddr = nullptr;
	bool bStylusProfileEnabled = false;
	TAtomic<bool> bAttached{false};
	TAtomic<bool> bProfileDirty{true};
	uint64 SyncCount = 0;

	XrActionSet ActionSet = XR_NULL_HANDLE;
	XrAction PoseActions[Pose_Count] = {};
	XrAction SelectAction = XR_NULL_HANDLE;
	XrAction MenuAction = XR_NULL_HANDLE;
	XrAction SystemAction = XR_NULL_HANDLE;
	XrAction TipForceAction = XR_NULL_HANDLE;
	XrAction MiddleForceAction = XR_NULL_HANDLE;
	XrAction FrontClickAction = XR_NULL_HANDLE;
	XrAction FrontDoubleTapAction = XR_NULL_HANDLE;
	XrAction BackClickAction = XR_NULL_HANDLE;
	XrAction BackDoubleTapAction = XR_NULL_HANDLE;
	XrAction DockedAction = XR_NULL_HANDLE;
	XrAction HapticAction = XR_NULL_HANDLE;

	XrPath HandPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrPath StylusProfilePath = XR_NULL_PATH;
	XrSpace Spaces[2][Pose_Count] = {};

	// Resolved once per instance.
	PFN_xrStringToPath xrStringToPathFunc = nullptr;
	PFN_xrPathToString xrPathToStringFunc = nullptr;
	PFN_xrCreateActionSet xrCreateActionSetFunc = nullptr;
	PFN_xrDestroyActionSet xrDestroyActionSetFunc = nullptr;
	PFN_xrCreateAction xrCreateActionFunc = nullptr;
	PFN_xrSuggestInteractionProfileBindings xrSuggestFunc = nullptr;
	PFN_xrAttachSessionActionSets xrAttachFunc = nullptr;
	PFN_xrCreateActionSpace xrCreateActionSpaceFunc = nullptr;
	PFN_xrDestroySpace xrDestroySpaceFunc = nullptr;
	PFN_xrSyncActions xrSyncActionsFunc = nullptr;
	PFN_xrGetActionStateBoolean xrGetBoolFunc = nullptr;
	PFN_xrGetActionStateFloat xrGetFloatFunc = nullptr;
	PFN_xrGetActionStatePose xrGetPoseFunc = nullptr;
	PFN_xrLocateSpace xrLocateSpaceFunc = nullptr;
	PFN_xrGetCurrentInteractionProfile xrGetCurrentProfileFunc = nullptr;
	PFN_xrApplyHapticFeedback xrApplyHapticFunc = nullptr;
	PFN_xrStopHapticFeedback xrStopHapticFunc = nullptr;

	// Published state. The lock is held only for the struct copy.
	mutable FCriticalSection SnapshotLock;
	FDisplayXRControllerSnapshot Published;
	// Game-thread working copy (carries the interaction profiles between syncs).
	FDisplayXRControllerSnapshot Working;
};
