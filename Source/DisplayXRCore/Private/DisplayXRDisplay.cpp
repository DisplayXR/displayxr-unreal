// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRDisplay.h"
#include "DisplayXRPlatform.h"
#include "DisplayXRRigManager.h"
#include "Camera/CameraComponent.h"

UDisplayXRDisplay::UDisplayXRDisplay()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UDisplayXRDisplay::OnRegister()
{
	Super::OnRegister();

	UCameraComponent* Camera = GetCamera();
	if (Camera)
	{
		FDisplayXRRigManager::Register(Camera, nullptr);
	}
}

void UDisplayXRDisplay::OnUnregister()
{
	UCameraComponent* Camera = GetCamera();
	if (Camera)
	{
		FDisplayXRRigManager::Unregister(Camera);
	}

	Super::OnUnregister();
}

void UDisplayXRDisplay::TickComponent(float DeltaTime, ELevelTick TickType,
                                       FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	PushTunables();
}

UCameraComponent* UDisplayXRDisplay::GetCamera() const
{
	if (AActor* Owner = GetOwner())
	{
		UCameraComponent* Camera = Owner->FindComponentByClass<UCameraComponent>();
		if (Camera)
		{
			return Camera;
		}
	}
	return nullptr;
}

void UDisplayXRDisplay::PushTunables()
{
	if (!FDisplayXRPlatform::IsAvailable())
	{
		return;
	}

	UCameraComponent* Camera = GetCamera();
	if (!Camera) return;
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn || !OwnerPawn->IsLocallyControlled()) return;

	// Build tunables
	FDisplayXRTunables T;
	T.IpdFactor = IpdFactor;
	T.ParallaxFactor = ParallaxFactor;
	T.PerspectiveFactor = PerspectiveFactor;
	T.VirtualDisplayHeight = VirtualDisplayHeight;
	T.NearZ = 0.1f;
	T.FarZ = 10000.0f;
	T.bCameraCentric = false;
	FDisplayXRPlatform::SetTunables(T);

	// Push the CAMERA's world transform (not the pawn's). UE's OpenXR stack
	// locates views relative to the camera's reference space, so the eye poses
	// returned by xrLocateViews rotate with the camera. Using the pawn's actor
	// transform would omit the camera rotation and break the eye → display-local
	// conversion whenever the camera yaws/pitches independently of the pawn.
	if (Camera)
	{
		FDisplayXRPlatform::SetSceneTransform(Camera->GetComponentTransform(), true);
	}
}
