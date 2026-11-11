// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRHandTracking.h"
#include "DisplayXRStereoMath.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"   // SDPG_Foreground
#include "DrawDebugHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRHands, Log, All);

#if UE_ENABLE_DEBUG_DRAWING
// Hand debug view — see Docs/DisplayXR/HandTracking.md. Read every tick, so it
// can be toggled from the console while playing.
static TAutoConsoleVariable<int32> CVarDisplayXRHandsDebug(
	TEXT("DisplayXR.Hands.Debug"),
	0,
	TEXT("Draw the tracked hand joints: one sphere per joint, both hands, at the joint radius, in the game world.\n")
	TEXT("0 (default): off.\n")
	TEXT("1: on. Left hand cyan, right hand orange, grey while a hand holds its last pose (out of the camera's view).\n")
	TEXT("Drawn at SDPG_Foreground so the spheres show in both views under TAA/TSR (SDPG_World debug lines are lost in the second view there, #70). Foreground lines draw over the scene."),
	ECVF_Default);
#endif

// UE's EHandKeypoint is declared in OpenXR joint order; the cache below indexes
// one by the other, so pin the correspondence (same checks Epic's plugin makes).
static_assert(EHandKeypointCount == XR_HAND_JOINT_COUNT_EXT, "EHandKeypoint count does not match XR_HAND_JOINT_COUNT_EXT.");
static_assert(EHandKeypoint::Palm == static_cast<EHandKeypoint>(XR_HAND_JOINT_PALM_EXT), "EHandKeypoint enum does not match XrHandJointEXT.");
static_assert(EHandKeypoint::Wrist == static_cast<EHandKeypoint>(XR_HAND_JOINT_WRIST_EXT), "EHandKeypoint enum does not match XrHandJointEXT.");
static_assert(EHandKeypoint::ThumbMetacarpal == static_cast<EHandKeypoint>(XR_HAND_JOINT_THUMB_METACARPAL_EXT), "EHandKeypoint enum does not match XrHandJointEXT.");
static_assert(EHandKeypoint::IndexTip == static_cast<EHandKeypoint>(XR_HAND_JOINT_INDEX_TIP_EXT), "EHandKeypoint enum does not match XrHandJointEXT.");
static_assert(EHandKeypoint::LittleTip == static_cast<EHandKeypoint>(XR_HAND_JOINT_LITTLE_TIP_EXT), "EHandKeypoint enum does not match XrHandJointEXT.");

FDisplayXRHandTracking::FDisplayXRHandTracking(FDisplayXRSession* InSession)
	: Session(InSession)
{
	IModularFeatures::Get().RegisterModularFeature(IHandTracker::GetModularFeatureName(), static_cast<IHandTracker*>(this));
	UE_LOG(LogDisplayXRHands, Log, TEXT("DisplayXR: IHandTracker registered (device type '%s')"), *DeviceTypeName().ToString());
#if UE_ENABLE_DEBUG_DRAWING
	PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FDisplayXRHandTracking::DrawDebugJoints);
#endif
}

FDisplayXRHandTracking::~FDisplayXRHandTracking()
{
#if UE_ENABLE_DEBUG_DRAWING
	FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
#endif
	IModularFeatures::Get().UnregisterModularFeature(IHandTracker::GetModularFeatureName(), static_cast<IHandTracker*>(this));
}

FName FDisplayXRHandTracking::DeviceTypeName()
{
	static const FName Name(TEXT("DisplayXR"));
	return Name;
}

FName FDisplayXRHandTracking::GetHandTrackerDeviceTypeName() const
{
	return DeviceTypeName();
}

int32 FDisplayXRHandTracking::HandIndex(EControllerHand Hand)
{
	switch (Hand)
	{
	case EControllerHand::Left:  return 0;
	case EControllerHand::Right: return 1;
	default:                     return -1;
	}
}

FTransform FDisplayXRHandTracking::TrackingToWorld()
{
	// Only our own device knows the rig; any other active XR system's
	// tracking origin has nothing to do with these joints.
	if (GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == DeviceTypeName())
	{
		return GEngine->XRSystem->GetTrackingToWorldTransform();
	}
	return FTransform::Identity;
}

#if UE_ENABLE_DEBUG_DRAWING
void FDisplayXRHandTracking::DrawDebugJoints(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	// Post-actor tick, i.e. where an actor's own Tick would draw: the lines
	// reach this frame's end-of-frame render update. The joints are the last
	// locate (the session locates in SetupViewFamily), as for any app reader.
	if (CVarDisplayXRHandsDebug.GetValueOnGameThread() == 0 || !World || !World->IsGameWorld())
	{
		return;
	}
	// Only our own device's rig places these joints in the world.
	if (!Session || !Session->HasHandTracking() || !GEngine || !GEngine->XRSystem.IsValid()
		|| GEngine->XRSystem->GetSystemName() != DeviceTypeName())
	{
		return;
	}
	RefreshCache();

	static const FColor HandColor[2] = {FColor(0, 200, 255), FColor(255, 140, 0)};
	static const FColor HeldColor(128, 128, 128);
	const FTransform ToWorld = TrackingToWorld();
	for (int32 H = 0; H < 2; ++H)
	{
		const FHandCache& C = Hands[H];
		if (!C.bEverTracked)
		{
			continue;
		}
		const FColor Color = C.bTracked ? HandColor[H] : HeldColor;
		for (int32 J = 0; J < EHandKeypointCount; ++J)
		{
			const FVector Loc = (C.Keypoints[J] * ToWorld).GetLocation();
			DrawDebugSphere(World, Loc, FMath::Max(C.Radii[J], 0.1f), 8, Color,
				/*bPersistentLines*/ false, /*LifeTime*/ -1.0f, SDPG_Foreground, /*Thickness*/ 0.0f);
		}
	}
}
#endif

void FDisplayXRHandTracking::RefreshCache() const
{
	if (!Session)
	{
		return;
	}

	if (Session->GetHandSampleGeneration() == CachedGeneration)
	{
		return;
	}
	FDisplayXRHandSample Samples[2];
	Session->GetHandSample(0, Samples[0]);
	Session->GetHandSample(1, Samples[1]);
	CachedGeneration = Samples[0].Generation;

	// --- Rig mapping: what is left to apply after the locate (see header) ---
	// A runtime with rig-scoped joints (XR_DXR_view_rig spec 4) already returned
	// them in the rig frame: identity. Otherwise they are display-plane metres
	// and the eyes' mapping is re-derived here (right only for a canvas centred
	// on the display). Both hands come from one locate pass, so they share a
	// frame; a hand that was never located has no joints to map.
	const FDisplayXRTunables T = Session->GetTunables();
	const FDisplayXRDisplayInfo DI = Session->GetDisplayInfo();
	const FDisplayXRRigRaw Raw = Session->GetRigRaw();

	const bool bRigFrame = Samples[0].bActive ? Samples[0].bRigFrame : Samples[1].bRigFrame;
	// Same fallbacks as the rig descriptor the session submits (LocateViews).
	const float Vdh = T.VirtualDisplayHeight > 0.0f
		? T.VirtualDisplayHeight
		: (DI.DisplayHeightMeters > 0.0f ? DI.DisplayHeightMeters : 0.194f);
	const float CanvasH = (Raw.bValid && Raw.CanvasHeightMeters > 0.0f)
		? Raw.CanvasHeightMeters
		: (DI.DisplayHeightMeters > 0.0f ? DI.DisplayHeightMeters : Vdh);
	const float NominalZ = DI.bIsValid ? (float)DI.NominalViewerPosition.Z : 0.5f;
	const dxr_pose::HandRigMapping Mapping =
		dxr_pose::HandRigMappingFor(bRigFrame, T.bCameraCentric, Vdh, CanvasH, NominalZ);
	const float Scale = Mapping.scale;
	const XrVector3f Offset = {0.0f, 0.0f, Mapping.offsetZ};

	// UE units per metre: the device reports 100 (DisplayXRDevice.cpp,
	// GetWorldToMetersScale); fall back to the same when it is not the active
	// system (nothing consumes the result then anyway).
	float WorldToMeters = 100.0f;
	if (GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == DeviceTypeName())
	{
		WorldToMeters = GEngine->XRSystem->GetWorldToMetersScale();
	}
	const float MetersToUnits = Scale * WorldToMeters;

	for (int32 H = 0; H < 2; ++H)
	{
		const FDisplayXRHandSample& S = Samples[H];
		FHandCache& C = Hands[H];
		C.bTracked = S.bActive;
		C.bEverTracked = S.bEverActive;
		if (!S.bActive)
		{
			// Keep the last converted set (same hold-last-pose behaviour as
			// the session, so a re-derive with new tunables is not needed).
			continue;
		}
		for (int32 J = 0; J < EHandKeypointCount; ++J)
		{
			const FDisplayXRHandSample::FJoint& Joint = S.Joints[J];
			const XrVector3f P = {
				Joint.Pose.position.x + Offset.x,
				Joint.Pose.position.y + Offset.y,
				Joint.Pose.position.z + Offset.z};
			C.Keypoints[J] = FTransform(
				OpenXROrientationToUE(Joint.Pose.orientation),
				OpenXRPositionToUE(P, MetersToUnits));
			C.Radii[J] = Joint.Radius * MetersToUnits;
		}
	}

	static int32 LogCount = 0;
	if (++LogCount <= 3 || LogCount % 600 == 0)
	{
		UE_LOG(LogDisplayXRHands, Verbose,
			TEXT("HandCache #%d: rig=%s frame=%s scale=%.3f offsetZ=%.3f L(tracked=%d) R(tracked=%d)"),
			LogCount, T.bCameraCentric ? TEXT("camera") : TEXT("display"),
			bRigFrame ? TEXT("rig (runtime-mapped)") : TEXT("display-plane (plugin-mapped)"), Scale, Offset.z,
			Hands[0].bTracked ? 1 : 0, Hands[1].bTracked ? 1 : 0);
	}
}

bool FDisplayXRHandTracking::IsHandTrackingStateValid() const
{
	if (!Session || !Session->HasHandTracking())
	{
		return false;
	}
	RefreshCache();
	return Hands[0].bTracked || Hands[1].bTracked;
}

bool FDisplayXRHandTracking::GetKeypointState(EControllerHand Hand, EHandKeypoint Keypoint,
	FTransform& OutTransform, float& OutRadius) const
{
	const int32 H = HandIndex(Hand);
	const int32 K = static_cast<int32>(Keypoint);
	if (!Session || !Session->HasHandTracking() || H < 0 || K < 0 || K >= EHandKeypointCount)
	{
		return false;
	}
	RefreshCache();
	const FHandCache& C = Hands[H];
	if (!C.bEverTracked)
	{
		return false;
	}
	OutTransform = C.Keypoints[K] * TrackingToWorld();
	OutRadius = C.Radii[K];
	return true;
}

bool FDisplayXRHandTracking::GetAllKeypointStates(EControllerHand Hand, TArray<FVector>& OutPositions,
	TArray<FQuat>& OutRotations, TArray<float>& OutRadii, bool& OutIsTracked) const
{
	return GetAllKeypointStatesInSpace(Hand, EXRSpaceType::UnrealWorldSpace,
		OutPositions, OutRotations, OutRadii, OutIsTracked);
}

bool FDisplayXRHandTracking::GetAllKeypointStatesInSpace(EControllerHand Hand, EXRSpaceType SpaceType,
	TArray<FVector>& OutPositions, TArray<FQuat>& OutRotations, TArray<float>& OutRadii,
	bool& OutIsTracked) const
{
	const int32 H = HandIndex(Hand);
	if (!Session || !Session->HasHandTracking() || H < 0)
	{
		return false;
	}
	RefreshCache();
	const FHandCache& C = Hands[H];
	if (!C.bEverTracked)
	{
		return false;
	}
	OutIsTracked = C.bTracked;

	const FTransform ToSpace = SpaceType == EXRSpaceType::UnrealWorldSpace
		? TrackingToWorld()
		: FTransform::Identity;

	OutPositions.Empty(EHandKeypointCount);
	OutRotations.Empty(EHandKeypointCount);
	OutRadii.Empty(EHandKeypointCount);
	for (int32 J = 0; J < EHandKeypointCount; ++J)
	{
		const FTransform W = C.Keypoints[J] * ToSpace;
		OutPositions.Add(W.GetLocation());
		OutRotations.Add(W.GetRotation());
		OutRadii.Add(C.Radii[J]);
	}
	return true;
}
