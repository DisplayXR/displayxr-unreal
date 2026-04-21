// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRComponentProxies.h"
#include "PrimitiveViewRelevance.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"

// ============================================================================
// Camera proxy
// ============================================================================

class FDisplayXRCameraSceneProxy : public FPrimitiveSceneProxy
{
public:
	FDisplayXRCameraSceneProxy(const UPrimitiveComponent* InComponent)
		: FPrimitiveSceneProxy(InComponent)
	{
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	                                     const FSceneViewFamily& ViewFamily,
	                                     uint32 VisibilityMap,
	                                     FMeshElementCollector& Collector) const override
	{
		// Draw convergence plane and frustum wireframe
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (!(VisibilityMap & (1 << ViewIndex)))
			{
				continue;
			}

			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
			const FMatrix& ProxyLocalToWorld = GetLocalToWorld();

			// Draw a simple frustum indicator
			const float FrustumLength = 100.0f; // cm
			const float HalfWidth = 50.0f;
			const float HalfHeight = 30.0f;

			FVector Origin = ProxyLocalToWorld.GetOrigin();
			FVector Forward = ProxyLocalToWorld.GetUnitAxis(EAxis::X) * FrustumLength;
			FVector Right = ProxyLocalToWorld.GetUnitAxis(EAxis::Y) * HalfWidth;
			FVector Up = ProxyLocalToWorld.GetUnitAxis(EAxis::Z) * HalfHeight;

			FVector FarTL = Origin + Forward - Right + Up;
			FVector FarTR = Origin + Forward + Right + Up;
			FVector FarBL = Origin + Forward - Right - Up;
			FVector FarBR = Origin + Forward + Right - Up;

			// Frustum edges
			FLinearColor Color(0.2f, 0.8f, 0.2f, 1.0f);
			PDI->DrawLine(Origin, FarTL, Color, SDPG_Foreground);
			PDI->DrawLine(Origin, FarTR, Color, SDPG_Foreground);
			PDI->DrawLine(Origin, FarBL, Color, SDPG_Foreground);
			PDI->DrawLine(Origin, FarBR, Color, SDPG_Foreground);

			// Far plane
			PDI->DrawLine(FarTL, FarTR, Color, SDPG_Foreground);
			PDI->DrawLine(FarTR, FarBR, Color, SDPG_Foreground);
			PDI->DrawLine(FarBR, FarBL, Color, SDPG_Foreground);
			PDI->DrawLine(FarBL, FarTL, Color, SDPG_Foreground);
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bDynamicRelevance = true;
		Result.bShadowRelevance = false;
		Result.bEditorPrimitiveRelevance = true;
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}
};

UDisplayXRCameraProxy::UDisplayXRCameraProxy()
{
	bIsEditorOnly = true;
	bHiddenInGame = true;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

FPrimitiveSceneProxy* UDisplayXRCameraProxy::CreateSceneProxy()
{
	return new FDisplayXRCameraSceneProxy(this);
}

FBoxSphereBounds UDisplayXRCameraProxy::CalcBounds(const FTransform& LocalToWorld) const
{
	return FBoxSphereBounds(FVector::ZeroVector, FVector(200.0f), 200.0f).TransformBy(LocalToWorld);
}

// ============================================================================
// Display proxy
// ============================================================================

class FDisplayXRDisplaySceneProxy : public FPrimitiveSceneProxy
{
public:
	FDisplayXRDisplaySceneProxy(const UPrimitiveComponent* InComponent)
		: FPrimitiveSceneProxy(InComponent)
	{
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	                                     const FSceneViewFamily& ViewFamily,
	                                     uint32 VisibilityMap,
	                                     FMeshElementCollector& Collector) const override
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (!(VisibilityMap & (1 << ViewIndex)))
			{
				continue;
			}

			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
			const FMatrix& ProxyLocalToWorld = GetLocalToWorld();

			// Draw display surface rectangle
			const float HalfWidth = 50.0f;
			const float HalfHeight = 30.0f;

			FVector Origin = ProxyLocalToWorld.GetOrigin();
			FVector Right = ProxyLocalToWorld.GetUnitAxis(EAxis::Y) * HalfWidth;
			FVector Up = ProxyLocalToWorld.GetUnitAxis(EAxis::Z) * HalfHeight;

			FVector TL = Origin - Right + Up;
			FVector TR = Origin + Right + Up;
			FVector BL = Origin - Right - Up;
			FVector BR = Origin + Right - Up;

			FLinearColor Color(0.2f, 0.4f, 0.9f, 1.0f);
			PDI->DrawLine(TL, TR, Color, SDPG_Foreground);
			PDI->DrawLine(TR, BR, Color, SDPG_Foreground);
			PDI->DrawLine(BR, BL, Color, SDPG_Foreground);
			PDI->DrawLine(BL, TL, Color, SDPG_Foreground);

			// Crosshair at center
			PDI->DrawLine(Origin - Right * 0.1f, Origin + Right * 0.1f, Color, SDPG_Foreground);
			PDI->DrawLine(Origin - Up * 0.1f, Origin + Up * 0.1f, Color, SDPG_Foreground);
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bDynamicRelevance = true;
		Result.bShadowRelevance = false;
		Result.bEditorPrimitiveRelevance = true;
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}
};

UDisplayXRDisplayProxy::UDisplayXRDisplayProxy()
{
	bIsEditorOnly = true;
	bHiddenInGame = true;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

FPrimitiveSceneProxy* UDisplayXRDisplayProxy::CreateSceneProxy()
{
	return new FDisplayXRDisplaySceneProxy(this);
}

FBoxSphereBounds UDisplayXRDisplayProxy::CalcBounds(const FTransform& LocalToWorld) const
{
	return FBoxSphereBounds(FVector::ZeroVector, FVector(200.0f), 200.0f).TransformBy(LocalToWorld);
}
