// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRCamera.h"
#include "Camera/CameraComponent.h"

void UDisplayXRCamera::BuildTunables(FDisplayXRTunables& T)
{
	UCameraComponent* Camera = GetCamera();
	if (!Camera)
	{
		return;
	}

	// Cache FOV on first use to avoid XR feedback loop
	if (!bFOVCached)
	{
		CachedFOV = Camera->FieldOfView;
		bFOVCached = true;
	}

	T.IpdFactor = IpdFactor;
	T.ParallaxFactor = ParallaxFactor;
	T.InvConvergenceDistance = InvConvergenceDistance;
	T.FovOverride = FMath::DegreesToRadians(CachedFOV);
	T.NearZ = Camera->OrthoNearClipPlane > 0.0f ? Camera->OrthoNearClipPlane * 0.01f : 0.1f;
	T.FarZ = 10000.0f;
	T.bCameraCentric = true;
}
