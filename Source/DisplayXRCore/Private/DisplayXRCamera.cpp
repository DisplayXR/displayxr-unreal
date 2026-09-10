// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRCamera.h"
#include "DisplayXRPlatform.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"
#include "UnrealClient.h"

// Vertical fov the way UE builds the 2D projection for this camera. FieldOfView is
// UE's horizontal angle; the vertical one depends on the local player's aspect-ratio
// axis constraint and the camera's own aspect settings, so reuse UE's function and
// read the vertical half-angle off the projection matrix (M[1][1] = 1 / tan(vfov/2)).
static float ComputeVerticalFov(const UCameraComponent* Camera, float FovDeg, const APawn* Pawn)
{
	if (Camera->ProjectionMode == ECameraProjectionMode::Orthographic)
	{
		return 0.0f; // rig default
	}

	FMinimalViewInfo ViewInfo;
	ViewInfo.FOV = FovDeg;
	ViewInfo.AspectRatio = Camera->AspectRatio;
	ViewInfo.bConstrainAspectRatio = Camera->bConstrainAspectRatio;
	ViewInfo.ProjectionMode = Camera->ProjectionMode;
	if (Camera->bOverrideAspectRatioAxisConstraint)
	{
		ViewInfo.AspectRatioAxisConstraint = Camera->AspectRatioAxisConstraint.GetValue();
	}

	TEnumAsByte<EAspectRatioAxisConstraint> Constraint = AspectRatio_MaintainYFOV; // engine default
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (const ULocalPlayer* LP = PC ? PC->GetLocalPlayer() : nullptr)
	{
		Constraint = LP->AspectRatioAxisConstraint;
	}

	// Reference rect = the canvas the stereo views are laid out in (window or editor zone).
	uint32 W = 0, H = 0;
	if (!FDisplayXRPlatform::GetViewCanvasSize(W, H) && GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		W = (uint32)Size.X;
		H = (uint32)Size.Y;
	}
	if (W == 0 || H == 0)
	{
		W = 1920;
		H = 1080;
	}

	FSceneViewProjectionData Proj;
	Proj.SetViewRectangle(FIntRect(0, 0, (int32)W, (int32)H));
	FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle(ViewInfo, Constraint, Proj.GetViewRect(), Proj);
	const float M11 = (float)Proj.ProjectionMatrix.M[1][1];
	return M11 > 0.0f ? 2.0f * FMath::Atan(1.0f / M11) : 0.0f;
}

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
	T.FovOverride = ComputeVerticalFov(Camera, CachedFOV, Cast<APawn>(GetOwner()));
	T.NearZ = Camera->OrthoNearClipPlane > 0.0f ? Camera->OrthoNearClipPlane * 0.01f : 0.1f;
	T.FarZ = 10000.0f;
	T.bCameraCentric = true;
}
