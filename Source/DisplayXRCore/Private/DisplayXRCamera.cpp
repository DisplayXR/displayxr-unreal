// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRCamera.h"
#include "DisplayXRPlatform.h"
#include "DisplayXRRigManager.h"
#include "Camera/CameraComponent.h"

UDisplayXRCamera::UDisplayXRCamera()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UDisplayXRCamera::OnRegister()
{
	Super::OnRegister();

	UCameraComponent* Camera = GetCamera();
	if (Camera)
	{
		FDisplayXRRigManager::Register(Camera, this);
	}
}

void UDisplayXRCamera::OnUnregister()
{
	UCameraComponent* Camera = GetCamera();
	if (Camera)
	{
		FDisplayXRRigManager::Unregister(Camera);
	}

	Super::OnUnregister();
}

void UDisplayXRCamera::TickComponent(float DeltaTime, ELevelTick TickType,
                                      FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	PushTunables();
}

UCameraComponent* UDisplayXRCamera::GetCamera() const
{
	if (AActor* Owner = GetOwner())
	{
		return Owner->FindComponentByClass<UCameraComponent>();
	}
	return nullptr;
}

void UDisplayXRCamera::PushTunables()
{
	if (!FDisplayXRPlatform::IsAvailable())
	{
		return;
	}

	// Only push if our owning pawn is currently possessed by a player
	UCameraComponent* Camera = GetCamera();
	if (!Camera) return;
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn || !OwnerPawn->IsLocallyControlled()) return;

	// Cache FOV on first tick to avoid XR feedback loop
	if (!bFOVCached)
	{
		CachedFOV = Camera->FieldOfView;
		bFOVCached = true;
	}

	// Build tunables
	FDisplayXRTunables T;
	T.IpdFactor = IpdFactor;
	T.ParallaxFactor = ParallaxFactor;
	T.InvConvergenceDistance = InvConvergenceDistance;
	T.FovOverride = FMath::DegreesToRadians(CachedFOV);
	T.NearZ = Camera->OrthoNearClipPlane > 0.0f ? Camera->OrthoNearClipPlane * 0.01f : 0.1f;
	T.FarZ = 10000.0f;
	T.bCameraCentric = true;
	FDisplayXRPlatform::SetTunables(T);

	// Push scene transform (camera world transform)
	FDisplayXRPlatform::SetSceneTransform(Camera->GetComponentTransform(), true);
}
