// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRInputFunctionLibrary.h"
#include "DisplayXRControllerTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DisplayXRInputFunctionLibrary)

namespace
{
	int32 DxrHandIndex(EControllerHand Hand)
	{
		switch (Hand)
		{
		case EControllerHand::Left:  return 0;
		case EControllerHand::Right: return 1;
		default:                     return -1;
		}
	}
}

bool UDisplayXRInputFunctionLibrary::IsDisplayXRControllerInputAvailable()
{
	return FDisplayXRControllerAPI::IsAvailable();
}

bool UDisplayXRInputFunctionLibrary::GetDisplayXRControllerInfo(EControllerHand Hand, bool& bActive, bool& bIsStylus, FString& InteractionProfile)
{
	bActive = false;
	bIsStylus = false;
	InteractionProfile.Reset();
	const int32 Index = DxrHandIndex(Hand);
	FDisplayXRControllerSnapshot Snap;
	if (Index < 0 || !FDisplayXRControllerAPI::GetSnapshot(Snap))
	{
		return false;
	}
	const FDisplayXRControllerState& S = Snap.Hands[Index];
	bActive = S.bActive;
	bIsStylus = S.bIsStylus;
	InteractionProfile = S.InteractionProfile;
	return true;
}

bool UDisplayXRInputFunctionLibrary::GetDisplayXRStylusForces(EControllerHand Hand, float& TipForce, float& MiddleForce)
{
	TipForce = 0.0f;
	MiddleForce = 0.0f;
	const int32 Index = DxrHandIndex(Hand);
	FDisplayXRControllerSnapshot Snap;
	if (Index < 0 || !FDisplayXRControllerAPI::GetSnapshot(Snap))
	{
		return false;
	}
	TipForce = Snap.Hands[Index].TipForce;
	MiddleForce = Snap.Hands[Index].MiddleForce;
	return true;
}
