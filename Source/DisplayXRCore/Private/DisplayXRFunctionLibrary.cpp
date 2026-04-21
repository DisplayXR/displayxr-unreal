// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRFunctionLibrary.h"
#include "DisplayXRPlatform.h"
#include "DisplayXRRigManager.h"

FDisplayXRDisplayInfo UDisplayXRFunctionLibrary::GetDisplayInfo()
{
	return FDisplayXRPlatform::GetDisplayInfo();
}

FVector UDisplayXRFunctionLibrary::GetUserPosition()
{
	FVector Left, Right;
	bool bTracked;
	FDisplayXRPlatform::GetEyePositions(Left, Right, bTracked);
	return (Left + Right) * 0.5f;
}

bool UDisplayXRFunctionLibrary::IsEyeTracked()
{
	FVector Left, Right;
	bool bTracked;
	FDisplayXRPlatform::GetEyePositions(Left, Right, bTracked);
	return bTracked;
}

bool UDisplayXRFunctionLibrary::RequestDisplayMode(bool bMode3D)
{
	return FDisplayXRPlatform::RequestDisplayMode(bMode3D);
}

bool UDisplayXRFunctionLibrary::RequestEyeTrackingMode(bool bManual)
{
	return FDisplayXRPlatform::RequestEyeTrackingMode(bManual);
}

void UDisplayXRFunctionLibrary::CycleRig()
{
	FDisplayXRRigManager::CycleNext();
}
