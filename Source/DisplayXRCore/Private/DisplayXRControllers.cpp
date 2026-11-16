// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRControllers.h"
#include "DisplayXRStereoMath.h"
#include "DisplayXRCoreModule.h"
#include "DisplayXRSession.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRControllers, Log, All);

namespace
{
	const char* const DxrSimpleControllerProfile = "/interaction_profiles/khr/simple_controller";
	const char* const DxrStylusProfile = "/interaction_profiles/logitech/mx_ink_stylus_logitech";

	template <typename T>
	void DxrResolveProc(PFN_xrGetInstanceProcAddr GetProcAddr, XrInstance Instance, const char* Name, T& Out)
	{
		Out = nullptr;
		if (GetProcAddr)
		{
			GetProcAddr(Instance, Name, reinterpret_cast<PFN_xrVoidFunction*>(&Out));
		}
	}

	FDisplayXRControllerPose DxrToUEControllerPose(const XrSpaceLocation& Loc, const dxr_pose::ControllerRigMapping& Mapping)
	{
		FDisplayXRControllerPose P;
		const XrSpaceLocationFlags ValidBits = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		const XrSpaceLocationFlags TrackedBits = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
		P.bValid = (Loc.locationFlags & ValidBits) == ValidBits;
		P.bTracked = P.bValid && (Loc.locationFlags & TrackedBits) == TrackedBits;
		if (P.bValid)
		{
			const XrPosef Rig = dxr_pose::MapControllerPoseToRig(Mapping, Loc.pose);
			// Metres in UE axes; the consumer applies its world-to-metres scale.
			P.PositionMeters = OpenXRPositionToUE(Rig.position, 1.0f);
			P.Orientation = OpenXROrientationToUE(Rig.orientation);
		}
		return P;
	}
}

XrPath FDisplayXRControllers::HandComponentPath(const char* Hand, const char* Component) const
{
	ANSICHAR Buffer[XR_MAX_PATH_LENGTH];
	FCStringAnsi::Snprintf(Buffer, XR_MAX_PATH_LENGTH, "%s%s", Hand, Component);
	return Path(Buffer);
}

XrPath FDisplayXRControllers::Path(const char* Str) const
{
	XrPath Out = XR_NULL_PATH;
	if (xrStringToPathFunc)
	{
		xrStringToPathFunc(Instance, Str, &Out);
	}
	return Out;
}

XrAction FDisplayXRControllers::CreateAction(const char* Name, const char* LocalizedName, XrActionType Type)
{
	XrActionCreateInfo Info = {XR_TYPE_ACTION_CREATE_INFO};
	FCStringAnsi::Strncpy(Info.actionName, Name, XR_MAX_ACTION_NAME_SIZE);
	FCStringAnsi::Strncpy(Info.localizedActionName, LocalizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
	Info.actionType = Type;
	Info.countSubactionPaths = 2;
	Info.subactionPaths = HandPaths;

	XrAction Action = XR_NULL_HANDLE;
	const XrResult Result = xrCreateActionFunc(ActionSet, &Info, &Action);
	if (XR_FAILED(Result))
	{
		UE_LOG(LogDisplayXRControllers, Warning, TEXT("DisplayXR Controllers: xrCreateAction(%s) failed (%d)"), ANSI_TO_TCHAR(Name), (int)Result);
		return XR_NULL_HANDLE;
	}
	return Action;
}

void FDisplayXRControllers::OnInstanceCreated(XrInstance InInstance, PFN_xrGetInstanceProcAddr InGetProcAddr, bool bInStylusProfileEnabled)
{
	Instance = InInstance;
	GetProcAddr = InGetProcAddr;
	bStylusProfileEnabled = bInStylusProfileEnabled;

	DxrResolveProc(GetProcAddr, Instance, "xrStringToPath", xrStringToPathFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrPathToString", xrPathToStringFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrCreateActionSet", xrCreateActionSetFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrDestroyActionSet", xrDestroyActionSetFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrCreateAction", xrCreateActionFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrSuggestInteractionProfileBindings", xrSuggestFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrAttachSessionActionSets", xrAttachFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrCreateActionSpace", xrCreateActionSpaceFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrDestroySpace", xrDestroySpaceFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrSyncActions", xrSyncActionsFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrGetActionStateBoolean", xrGetBoolFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrGetActionStateFloat", xrGetFloatFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrGetActionStatePose", xrGetPoseFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrLocateSpace", xrLocateSpaceFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrGetCurrentInteractionProfile", xrGetCurrentProfileFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrApplyHapticFeedback", xrApplyHapticFunc);
	DxrResolveProc(GetProcAddr, Instance, "xrStopHapticFeedback", xrStopHapticFunc);

	if (!xrStringToPathFunc || !xrCreateActionSetFunc || !xrCreateActionFunc || !xrSuggestFunc || !xrAttachFunc
		|| !xrCreateActionSpaceFunc || !xrSyncActionsFunc || !xrGetBoolFunc || !xrGetFloatFunc || !xrGetPoseFunc
		|| !xrLocateSpaceFunc)
	{
		UE_LOG(LogDisplayXRControllers, Warning, TEXT("DisplayXR Controllers: the runtime does not expose the OpenXR action API — controllers disabled"));
		return;
	}

	HandPaths[0] = Path("/user/hand/left");
	HandPaths[1] = Path("/user/hand/right");
	StylusProfilePath = Path(DxrStylusProfile);

	XrActionSetCreateInfo SetInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
	FCStringAnsi::Strncpy(SetInfo.actionSetName, "displayxr_input", XR_MAX_ACTION_SET_NAME_SIZE);
	FCStringAnsi::Strncpy(SetInfo.localizedActionSetName, "DisplayXR Input", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
	SetInfo.priority = 0;
	XrResult Result = xrCreateActionSetFunc(Instance, &SetInfo, &ActionSet);
	if (XR_FAILED(Result))
	{
		UE_LOG(LogDisplayXRControllers, Warning, TEXT("DisplayXR Controllers: xrCreateActionSet failed (%d) — controllers disabled"), (int)Result);
		ActionSet = XR_NULL_HANDLE;
		return;
	}

	PoseActions[Pose_Grip] = CreateAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT);
	PoseActions[Pose_Aim] = CreateAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
	PoseActions[Pose_Tip] = CreateAction("tip_pose", "Tip Pose", XR_ACTION_TYPE_POSE_INPUT);
	SelectAction = CreateAction("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT);
	MenuAction = CreateAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
	SystemAction = CreateAction("system", "System", XR_ACTION_TYPE_BOOLEAN_INPUT);
	TipForceAction = CreateAction("tip_force", "Tip Force", XR_ACTION_TYPE_FLOAT_INPUT);
	MiddleForceAction = CreateAction("middle_force", "Middle Force", XR_ACTION_TYPE_FLOAT_INPUT);
	FrontClickAction = CreateAction("front_click", "Front Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
	FrontDoubleTapAction = CreateAction("front_double_tap", "Front Button Double Tap", XR_ACTION_TYPE_BOOLEAN_INPUT);
	BackClickAction = CreateAction("back_click", "Back Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
	BackDoubleTapAction = CreateAction("back_double_tap", "Back Button Double Tap", XR_ACTION_TYPE_BOOLEAN_INPUT);
	DockedAction = CreateAction("docked", "Docked", XR_ACTION_TYPE_BOOLEAN_INPUT);
	HapticAction = CreateAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

	const bool bSimple = SuggestSimpleController();
	const bool bStylus = bStylusProfileEnabled && SuggestStylus();
	UE_LOG(LogDisplayXRControllers, Log,
		TEXT("DisplayXR Controllers: action set created; bindings suggested: simple_controller=%s, mx_ink_stylus=%s"),
		bSimple ? TEXT("yes") : TEXT("FAILED"),
		bStylusProfileEnabled ? (bStylus ? TEXT("yes") : TEXT("FAILED")) : TEXT("no (extension not advertised)"));
}

bool FDisplayXRControllers::Suggest(const char* Profile, const TArray<XrActionSuggestedBinding>& Bindings)
{
	XrInteractionProfileSuggestedBinding Suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	Suggested.interactionProfile = Path(Profile);
	Suggested.countSuggestedBindings = (uint32_t)Bindings.Num();
	Suggested.suggestedBindings = Bindings.GetData();
	const XrResult Result = xrSuggestFunc(Instance, &Suggested);
	if (XR_FAILED(Result))
	{
		UE_LOG(LogDisplayXRControllers, Log, TEXT("DisplayXR Controllers: xrSuggestInteractionProfileBindings(%s, %d bindings) failed (%d)"),
			ANSI_TO_TCHAR(Profile), Bindings.Num(), (int)Result);
		return false;
	}
	return true;
}

bool FDisplayXRControllers::SuggestSimpleController()
{
	TArray<XrActionSuggestedBinding> B;
	for (const char* Hand : {"/user/hand/left", "/user/hand/right"})
	{
		auto Add = [&](XrAction Action, const char* Component)
		{
			if (Action != XR_NULL_HANDLE)
			{
				B.Add({Action, HandComponentPath(Hand, Component)});
			}
		};
		Add(PoseActions[Pose_Grip], "/input/grip/pose");
		Add(PoseActions[Pose_Aim], "/input/aim/pose");
		Add(SelectAction, "/input/select/click");
		Add(MenuAction, "/input/menu/click");
		Add(HapticAction, "/output/haptic");
	}
	return Suggest(DxrSimpleControllerProfile, B);
}

bool FDisplayXRControllers::SuggestStylus()
{
	// Tiered: a runtime may serve the profile without every optional component
	// (any unsupported path fails the whole call with XR_ERROR_PATH_UNSUPPORTED),
	// so retry with less rather than lose the stylus.
	for (int32 Tier = 0; Tier < 3; ++Tier)
	{
		TArray<XrActionSuggestedBinding> B;
		for (const char* Hand : {"/user/hand/left", "/user/hand/right"})
		{
			auto Add = [&](XrAction Action, const char* Component)
			{
				if (Action != XR_NULL_HANDLE)
				{
					B.Add({Action, HandComponentPath(Hand, Component)});
				}
			};
			// Tier 2 (minimum): poses + tip + haptic.
			Add(PoseActions[Pose_Grip], "/input/grip/pose");
			Add(PoseActions[Pose_Aim], "/input/aim/pose");
			Add(PoseActions[Pose_Tip], "/input/tip_logitech/pose");
			Add(TipForceAction, "/input/tip_logitech/force");
			Add(SelectAction, "/input/tip_logitech/force");
			Add(HapticAction, "/output/haptic");
			if (Tier <= 1)
			{
				Add(MiddleForceAction, "/input/cluster_middle_logitech/force");
				Add(FrontClickAction, "/input/cluster_front_logitech/click");
				Add(BackClickAction, "/input/cluster_back_logitech/click");
			}
			if (Tier == 0)
			{
				Add(FrontDoubleTapAction, "/input/cluster_front_logitech/double_tap_logitech");
				Add(BackDoubleTapAction, "/input/cluster_back_logitech/double_tap_logitech");
				Add(DockedAction, "/input/dock_logitech/docked_logitech");
				Add(SystemAction, "/input/system/click");
			}
		}
		if (Suggest(DxrStylusProfile, B))
		{
			if (Tier > 0)
			{
				UE_LOG(LogDisplayXRControllers, Warning,
					TEXT("DisplayXR Controllers: stylus bindings accepted only at reduced tier %d (%s)"), Tier,
					Tier == 1 ? TEXT("no double taps / dock / system") : TEXT("poses, tip and haptics only"));
			}
			return true;
		}
	}
	return false;
}

void FDisplayXRControllers::OnSessionCreated(XrSession InSession)
{
	Session = InSession;
	bAttached.Store(false);
	if (ActionSet == XR_NULL_HANDLE || Session == XR_NULL_HANDLE)
	{
		return;
	}

	XrSessionActionSetsAttachInfo AttachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	AttachInfo.countActionSets = 1;
	AttachInfo.actionSets = &ActionSet;
	const XrResult Result = xrAttachFunc(Session, &AttachInfo);
	if (XR_FAILED(Result))
	{
		UE_LOG(LogDisplayXRControllers, Warning, TEXT("DisplayXR Controllers: xrAttachSessionActionSets failed (%d) — controllers disabled for this session"), (int)Result);
		return;
	}

	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		for (int32 P = 0; P < Pose_Count; ++P)
		{
			Spaces[Hand][P] = XR_NULL_HANDLE;
			if (PoseActions[P] == XR_NULL_HANDLE)
			{
				continue;
			}
			XrActionSpaceCreateInfo SpaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			SpaceInfo.action = PoseActions[P];
			SpaceInfo.subactionPath = HandPaths[Hand];
			SpaceInfo.poseInActionSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
			SpaceInfo.poseInActionSpace.position = {0.0f, 0.0f, 0.0f};
			const XrResult SR = xrCreateActionSpaceFunc(Session, &SpaceInfo, &Spaces[Hand][P]);
			if (XR_FAILED(SR))
			{
				UE_LOG(LogDisplayXRControllers, Warning, TEXT("DisplayXR Controllers: xrCreateActionSpace(hand %d, pose %d) failed (%d)"), Hand, P, (int)SR);
				Spaces[Hand][P] = XR_NULL_HANDLE;
			}
		}
	}

	Working = FDisplayXRControllerSnapshot();
	bProfileDirty.Store(true);
	bAttached.Store(true);
	UE_LOG(LogDisplayXRControllers, Log, TEXT("DisplayXR Controllers: action set attached to the session"));
}

void FDisplayXRControllers::DestroySpaces()
{
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		for (int32 P = 0; P < Pose_Count; ++P)
		{
			if (Spaces[Hand][P] != XR_NULL_HANDLE && xrDestroySpaceFunc)
			{
				xrDestroySpaceFunc(Spaces[Hand][P]);
			}
			Spaces[Hand][P] = XR_NULL_HANDLE;
		}
	}
}

void FDisplayXRControllers::OnSessionDestroying()
{
	bAttached.Store(false);
	DestroySpaces();
	Session = XR_NULL_HANDLE;
	{
		FScopeLock Lock(&SnapshotLock);
		Published = FDisplayXRControllerSnapshot();
	}
	Working = FDisplayXRControllerSnapshot();
}

void FDisplayXRControllers::OnInstanceDestroying()
{
	OnSessionDestroying();
	if (ActionSet != XR_NULL_HANDLE && xrDestroyActionSetFunc)
	{
		xrDestroyActionSetFunc(ActionSet); // destroys its actions too
	}
	ActionSet = XR_NULL_HANDLE;
	for (XrAction& A : PoseActions)
	{
		A = XR_NULL_HANDLE;
	}
	SelectAction = MenuAction = SystemAction = TipForceAction = MiddleForceAction = XR_NULL_HANDLE;
	FrontClickAction = FrontDoubleTapAction = BackClickAction = BackDoubleTapAction = DockedAction = HapticAction = XR_NULL_HANDLE;
	Instance = XR_NULL_HANDLE;
	GetProcAddr = nullptr;
}

void FDisplayXRControllers::RefreshInteractionProfiles(FDisplayXRControllerSnapshot& Snap)
{
	if (!xrGetCurrentProfileFunc || !xrPathToStringFunc)
	{
		return;
	}
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		XrInteractionProfileState State = {XR_TYPE_INTERACTION_PROFILE_STATE};
		FString Profile;
		bool bStylus = false;
		if (XR_SUCCEEDED(xrGetCurrentProfileFunc(Session, HandPaths[Hand], &State)) && State.interactionProfile != XR_NULL_PATH)
		{
			char Buffer[XR_MAX_PATH_LENGTH] = {};
			uint32_t Count = 0;
			if (XR_SUCCEEDED(xrPathToStringFunc(Instance, State.interactionProfile, XR_MAX_PATH_LENGTH, &Count, Buffer)))
			{
				Profile = ANSI_TO_TCHAR(Buffer);
			}
			bStylus = State.interactionProfile == StylusProfilePath;
		}
		if (Profile != Snap.Hands[Hand].InteractionProfile)
		{
			UE_LOG(LogDisplayXRControllers, Log, TEXT("DisplayXR Controllers: %s hand interaction profile: %s"),
				Hand == 0 ? TEXT("left") : TEXT("right"), Profile.IsEmpty() ? TEXT("(none)") : *Profile);
		}
		Snap.Hands[Hand].InteractionProfile = Profile;
		Snap.Hands[Hand].bIsStylus = bStylus;
	}
}

void FDisplayXRControllers::SyncAndLocate(XrSpace BaseSpace, XrTime Time, const dxr_pose::ControllerRigMapping& Mapping)
{
	if (!bAttached.Load() || Session == XR_NULL_HANDLE || BaseSpace == XR_NULL_HANDLE || Time == 0)
	{
		return;
	}

	XrActiveActionSet Active = {ActionSet, XR_NULL_PATH};
	XrActionsSyncInfo SyncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
	SyncInfo.countActiveActionSets = 1;
	SyncInfo.activeActionSets = &Active;
	const XrResult SyncResult = xrSyncActionsFunc(Session, &SyncInfo);
	if (XR_FAILED(SyncResult))
	{
		static int32 FailCount = 0;
		if (++FailCount <= 3 || FailCount % 600 == 0)
		{
			UE_LOG(LogDisplayXRControllers, Warning, TEXT("DisplayXR Controllers: xrSyncActions failed #%d (%d)"), FailCount, (int)SyncResult);
		}
		return;
	}

	FDisplayXRControllerSnapshot& Snap = Working;
	++SyncCount;
	// The profile-changed event is the trigger; a slow re-query (~2 s at
	// 60 fps) covers a runtime that does not queue it.
	if (bProfileDirty.Exchange(false) || (SyncCount % 120) == 1)
	{
		RefreshInteractionProfiles(Snap);
	}

	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		FDisplayXRControllerState& S = Snap.Hands[Hand];
		const XrPath Sub = HandPaths[Hand];

		auto GetBool = [&](XrAction Action, bool& bOutActive) -> bool
		{
			if (Action == XR_NULL_HANDLE)
			{
				return false;
			}
			XrActionStateGetInfo Info = {XR_TYPE_ACTION_STATE_GET_INFO};
			Info.action = Action;
			Info.subactionPath = Sub;
			XrActionStateBoolean State = {XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_FAILED(xrGetBoolFunc(Session, &Info, &State)) || State.isActive != XR_TRUE)
			{
				return false;
			}
			bOutActive = true;
			return State.currentState == XR_TRUE;
		};
		auto GetFloat = [&](XrAction Action, bool& bOutActive) -> float
		{
			if (Action == XR_NULL_HANDLE)
			{
				return 0.0f;
			}
			XrActionStateGetInfo Info = {XR_TYPE_ACTION_STATE_GET_INFO};
			Info.action = Action;
			Info.subactionPath = Sub;
			XrActionStateFloat State = {XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetFloatFunc(Session, &Info, &State)) || State.isActive != XR_TRUE)
			{
				return 0.0f;
			}
			bOutActive = true;
			return FMath::Clamp(State.currentState, 0.0f, 1.0f);
		};

		bool bAnyActive = false;
		S.bSelect = GetBool(SelectAction, bAnyActive);
		S.bMenu = GetBool(MenuAction, bAnyActive);
		S.bSystemClick = GetBool(SystemAction, bAnyActive);
		S.bFrontClick = GetBool(FrontClickAction, bAnyActive);
		S.bFrontDoubleTap = GetBool(FrontDoubleTapAction, bAnyActive);
		S.bBackClick = GetBool(BackClickAction, bAnyActive);
		S.bBackDoubleTap = GetBool(BackDoubleTapAction, bAnyActive);
		S.bDocked = GetBool(DockedAction, bAnyActive);
		S.TipForce = GetFloat(TipForceAction, bAnyActive);
		S.MiddleForce = GetFloat(MiddleForceAction, bAnyActive);

		FDisplayXRControllerPose* Poses[Pose_Count] = {&S.Grip, &S.Aim, &S.Tip};
		for (int32 P = 0; P < Pose_Count; ++P)
		{
			*Poses[P] = FDisplayXRControllerPose();
			if (Spaces[Hand][P] == XR_NULL_HANDLE)
			{
				continue;
			}
			XrActionStateGetInfo Info = {XR_TYPE_ACTION_STATE_GET_INFO};
			Info.action = PoseActions[P];
			Info.subactionPath = Sub;
			XrActionStatePose PoseState = {XR_TYPE_ACTION_STATE_POSE};
			if (XR_FAILED(xrGetPoseFunc(Session, &Info, &PoseState)) || PoseState.isActive != XR_TRUE)
			{
				continue;
			}
			bAnyActive = true;
			XrSpaceLocation Loc = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpaceFunc(Spaces[Hand][P], BaseSpace, Time, &Loc)))
			{
				*Poses[P] = DxrToUEControllerPose(Loc, Mapping);
			}
		}
		S.bActive = bAnyActive;
	}
	Snap.Generation = SyncCount;

	{
		FScopeLock Lock(&SnapshotLock);
		Published = Snap;
	}

	if (SyncCount <= 3 || SyncCount % 600 == 0)
	{
		const FDisplayXRControllerState& L = Snap.Hands[0];
		const FDisplayXRControllerState& R = Snap.Hands[1];
		UE_LOG(LogDisplayXRControllers, Log,
			TEXT("Controllers #%llu: L(active=%d aim=%d %.3f,%.3f,%.3f) R(active=%d aim=%d %.3f,%.3f,%.3f) gain=%.3f"),
			(unsigned long long)SyncCount,
			L.bActive ? 1 : 0, L.Aim.bValid ? 1 : 0, L.Aim.PositionMeters.X, L.Aim.PositionMeters.Y, L.Aim.PositionMeters.Z,
			R.bActive ? 1 : 0, R.Aim.bValid ? 1 : 0, R.Aim.PositionMeters.X, R.Aim.PositionMeters.Y, R.Aim.PositionMeters.Z,
			Mapping.scale);
	}
}

void FDisplayXRControllers::GetSnapshot(FDisplayXRControllerSnapshot& Out) const
{
	FScopeLock Lock(&SnapshotLock);
	Out = Published;
}

bool FDisplayXRControllers::ApplyHaptic(int32 HandIndex, float Amplitude, float FrequencyHz, float DurationSeconds)
{
	if (!bAttached.Load() || HapticAction == XR_NULL_HANDLE || !xrApplyHapticFunc || HandIndex < 0 || HandIndex > 1)
	{
		return false;
	}
	XrHapticActionInfo Info = {XR_TYPE_HAPTIC_ACTION_INFO};
	Info.action = HapticAction;
	Info.subactionPath = HandPaths[HandIndex];

	XrHapticVibration Vibration = {XR_TYPE_HAPTIC_VIBRATION};
	Vibration.amplitude = FMath::Clamp(Amplitude, 0.0f, 1.0f);
	Vibration.frequency = FrequencyHz > 0.0f ? FrequencyHz : XR_FREQUENCY_UNSPECIFIED;
	Vibration.duration = DurationSeconds > 0.0f ? (XrDuration)((double)DurationSeconds * 1e9) : XR_MIN_HAPTIC_DURATION;
	return XR_SUCCEEDED(xrApplyHapticFunc(Session, &Info, reinterpret_cast<const XrHapticBaseHeader*>(&Vibration)));
}

void FDisplayXRControllers::StopHaptic(int32 HandIndex)
{
	if (!bAttached.Load() || HapticAction == XR_NULL_HANDLE || !xrStopHapticFunc || HandIndex < 0 || HandIndex > 1)
	{
		return;
	}
	XrHapticActionInfo Info = {XR_TYPE_HAPTIC_ACTION_INFO};
	Info.action = HapticAction;
	Info.subactionPath = HandPaths[HandIndex];
	xrStopHapticFunc(Session, &Info);
}

// =============================================================================
// FDisplayXRControllerAPI (public entry points)
// =============================================================================

namespace
{
	FDisplayXRControllers* DxrActiveControllers()
	{
		FDisplayXRSession* Session = FDisplayXRCoreModule::GetSession();
		return Session ? Session->GetControllers() : nullptr;
	}
}

bool FDisplayXRControllerAPI::IsAvailable()
{
	const FDisplayXRControllers* C = DxrActiveControllers();
	return C && C->IsAttached();
}

bool FDisplayXRControllerAPI::IsStylusProfileEnabled()
{
	const FDisplayXRControllers* C = DxrActiveControllers();
	return C && C->IsStylusProfileEnabled();
}

bool FDisplayXRControllerAPI::GetSnapshot(FDisplayXRControllerSnapshot& Out)
{
	const FDisplayXRControllers* C = DxrActiveControllers();
	if (!C || !C->IsAttached())
	{
		Out = FDisplayXRControllerSnapshot();
		return false;
	}
	C->GetSnapshot(Out);
	return Out.Generation != 0;
}

bool FDisplayXRControllerAPI::ApplyHaptic(int32 HandIndex, float Amplitude, float FrequencyHz, float DurationSeconds)
{
	FDisplayXRControllers* C = DxrActiveControllers();
	return C && C->ApplyHaptic(HandIndex, Amplitude, FrequencyHz, DurationSeconds);
}

void FDisplayXRControllerAPI::StopHaptic(int32 HandIndex)
{
	if (FDisplayXRControllers* C = DxrActiveControllers())
	{
		C->StopHaptic(HandIndex);
	}
}
