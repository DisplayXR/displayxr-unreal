// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRRigVisualizers.h"
#include "DisplayXRCamera.h"
#include "DisplayXRDisplay.h"
#include "DisplayXRPlatform.h"
#include "Camera/CameraComponent.h"
#include "SceneManagement.h"

namespace
{
	const FLinearColor ConvergenceColor(1.0f, 0.6f, 0.2f);   // finite convergence (matches the Unity gizmo)
	const FLinearColor ParallelColor(0.7f, 0.4f, 1.0f);      // inverse distance 0 = parallel projection
	const FLinearColor DisplayPlaneColor(0.2f, 0.8f, 1.0f);
	constexpr float ParallelPreviewDistanceCm = 200.0f;

	// Wire rectangle centered at Center in the plane spanned by Right/Up, a small
	// center cross, and optionally edges from Apex to the corners.
	void DrawPlane(FPrimitiveDrawInterface* PDI, const FVector& Center, const FVector& Right, const FVector& Up,
		float HalfW, float HalfH, const FLinearColor& Color, const FVector* Apex)
	{
		const FVector C[4] = {
			Center - Right * HalfW - Up * HalfH,
			Center + Right * HalfW - Up * HalfH,
			Center + Right * HalfW + Up * HalfH,
			Center - Right * HalfW + Up * HalfH,
		};
		for (int32 i = 0; i < 4; ++i)
		{
			PDI->DrawLine(C[i], C[(i + 1) % 4], Color, SDPG_World, 1.5f);
			if (Apex)
			{
				PDI->DrawLine(*Apex, C[i], Color * 0.6f, SDPG_World, 0.5f);
			}
		}
		const float Tick = FMath::Min(HalfW, HalfH) * 0.1f;
		PDI->DrawLine(Center - Right * Tick, Center + Right * Tick, Color, SDPG_World, 1.0f);
		PDI->DrawLine(Center - Up * Tick, Center + Up * Tick, Color, SDPG_World, 1.0f);
	}
}

void FDisplayXRCameraVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UDisplayXRCamera* Rig = Cast<UDisplayXRCamera>(Component);
	const UCameraComponent* Camera = Rig ? Rig->GetCamera() : nullptr;
	if (!Camera || !PDI)
	{
		return;
	}

	const bool bParallel = Rig->InvConvergenceDistance <= KINDA_SMALL_NUMBER;
	// InvConvergenceDistance is 1/m (metersToVirtual = 1); UE world units are cm.
	const float Distance = bParallel ? ParallelPreviewDistanceCm : 100.0f / Rig->InvConvergenceDistance;

	// FieldOfView is UE's horizontal angle; the vertical follows the camera's
	// AspectRatio, the same rule UE's own camera frustum gizmo uses.
	const float HalfW = Distance * FMath::Tan(FMath::DegreesToRadians(Camera->FieldOfView) * 0.5f);
	const float HalfH = HalfW / FMath::Max(Camera->AspectRatio, 0.01f);

	const FTransform& T = Camera->GetComponentTransform();
	const FVector Apex = T.GetLocation();
	DrawPlane(PDI, Apex + T.GetUnitAxis(EAxis::X) * Distance, T.GetUnitAxis(EAxis::Y), T.GetUnitAxis(EAxis::Z),
		HalfW, HalfH, bParallel ? ParallelColor : ConvergenceColor, &Apex);
}

void FDisplayXRDisplayVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UDisplayXRDisplay* Rig = Cast<UDisplayXRDisplay>(Component);
	const UCameraComponent* Camera = Rig ? Rig->GetCamera() : nullptr;
	if (!Camera || !PDI)
	{
		return;
	}

	// Display plane extents: VirtualDisplayHeight when set, else the physical display
	// the runtime reports, else a 16:9 placeholder.
	const FDisplayXRDisplayInfo Info = FDisplayXRPlatform::GetDisplayInfo();
	const bool bPhysical = Info.bIsValid && Info.DisplayHeightMeters > 0.0f;
	const float Aspect = bPhysical ? Info.DisplayWidthMeters / Info.DisplayHeightMeters : 16.0f / 9.0f;
	const float HeightCm = Rig->VirtualDisplayHeight > 0.0f ? Rig->VirtualDisplayHeight
		: (bPhysical ? Info.DisplayHeightMeters * 100.0f : 30.0f);
	const float HalfH = HeightCm * 0.5f;

	// The camera transform is the display plane.
	const FTransform& T = Camera->GetComponentTransform();
	DrawPlane(PDI, T.GetLocation(), T.GetUnitAxis(EAxis::Y), T.GetUnitAxis(EAxis::Z), HalfH * Aspect, HalfH, DisplayPlaneColor, nullptr);
}
