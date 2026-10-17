// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRHandTracking.h"
#include "DisplayXRStereoMath.h"
#include "Features/IModularFeatures.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRHands, Log, All);

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
}

FDisplayXRHandTracking::~FDisplayXRHandTracking()
{
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

	// --- Rig mapping: display-plane metres -> rig-local metres (see header) ---
	const FDisplayXRTunables T = Session->GetTunables();
	const FDisplayXRDisplayInfo DI = Session->GetDisplayInfo();
	const FDisplayXRRigRaw Raw = Session->GetRigRaw();

	float Scale = 1.0f;
	XrVector3f Offset = {0.0f, 0.0f, 0.0f};
	if (T.bCameraCentric)
	{
		// Camera rig: eye_local = m2v * (eye - (0, 0, nominalZ)); the session
		// passes metersToVirtual = 1, so m2v == 1 here too.
		const float NominalZ = DI.bIsValid ? (float)DI.NominalViewerPosition.Z : 0.5f;
		Offset.z = -NominalZ;
	}
	else
	{
		// Display rig: the eyes were scaled by m2v = vdh / canvas height. Same
		// fallbacks as the rig descriptor the session submits (LocateViews).
		const float Vdh = T.VirtualDisplayHeight > 0.0f
			? T.VirtualDisplayHeight
			: (DI.DisplayHeightMeters > 0.0f ? DI.DisplayHeightMeters : 0.194f);
		const float CanvasH = (Raw.bValid && Raw.CanvasHeightMeters > 0.0f)
			? Raw.CanvasHeightMeters
			: (DI.DisplayHeightMeters > 0.0f ? DI.DisplayHeightMeters : Vdh);
		Scale = CanvasH > 0.0f ? Vdh / CanvasH : 1.0f;
	}

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
			TEXT("HandCache #%d: rig=%s scale=%.3f offsetZ=%.3f L(tracked=%d) R(tracked=%d)"),
			LogCount, T.bCameraCentric ? TEXT("camera") : TEXT("display"), Scale, Offset.z,
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
