// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRRigManager.h"
#include "DisplayXRPlatform.h"
#include "DisplayXRRigComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

TArray<TWeakObjectPtr<UDisplayXRRigComponent>> FDisplayXRRigManager::RegisteredRigs;
TArray<TWeakObjectPtr<UCameraComponent>> FDisplayXRRigManager::RegisteredCameras;
TWeakObjectPtr<UCameraComponent> FDisplayXRRigManager::ActiveCamera;

void FDisplayXRRigManager::RegisterRig(UDisplayXRRigComponent* Rig)
{
	if (!Rig)
	{
		return;
	}
	RegisteredRigs.AddUnique(Rig);
	Register(Rig->GetCamera());
}

void FDisplayXRRigManager::UnregisterRig(UDisplayXRRigComponent* Rig)
{
	RegisteredRigs.RemoveAll([Rig](const TWeakObjectPtr<UDisplayXRRigComponent>& Weak)
	{
		return !Weak.IsValid() || Weak.Get() == Rig;
	});
	if (Rig)
	{
		Unregister(Rig->GetCamera());
	}
}

UCameraComponent* FDisplayXRRigManager::FindViewCamera(const UWorld* World)
{
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AActor* ViewTarget = (PC && PC->PlayerCameraManager) ? PC->PlayerCameraManager->GetViewTarget() : nullptr;
	if (!ViewTarget || !ViewTarget->bFindCameraComponentWhenViewTarget)
	{
		return nullptr; // AActor::CalcCamera renders from the eyes viewpoint, not a camera
	}

	// Same rule as AActor::CalcCamera: the first active camera component wins.
	TInlineComponentArray<UCameraComponent*> Cameras;
	ViewTarget->GetComponents(Cameras);
	for (UCameraComponent* Camera : Cameras)
	{
		if (Camera->IsActive())
		{
			return Camera;
		}
	}
	return nullptr;
}

void FDisplayXRRigManager::PushActiveRig(const UWorld* World)
{
	if (!World || !FDisplayXRPlatform::IsAvailable())
	{
		return;
	}

	UCameraComponent* ViewCamera = FindViewCamera(World);
	if (!ViewCamera)
	{
		return;
	}

	UDisplayXRRigComponent* Rig = nullptr;
	for (const TWeakObjectPtr<UDisplayXRRigComponent>& Weak : RegisteredRigs)
	{
		UDisplayXRRigComponent* Candidate = Weak.Get();
		if (Candidate && Candidate->GetWorld() == World && Candidate->GetCamera() == ViewCamera)
		{
			Rig = Candidate;
			break;
		}
	}

	// The session outlives PIE and keeps the last push, so a view camera without a
	// rig must reset to defaults rather than inherit the previous rig's tunables
	// (e.g. a Sequencer cut to a bare ACameraActor, or a rig-less level next Play).
	FDisplayXRTunables T;
	if (Rig)
	{
		Rig->BuildTunables(T);
	}
	FDisplayXRPlatform::SetTunables(T);

	// Diagnostic only since #396 W7: the view rig is submitted with an identity
	// pose (UE applies camera placement itself); the session logs this alongside it.
	FDisplayXRPlatform::SetSceneTransform(ViewCamera->GetComponentTransform(), true);
}

void FDisplayXRRigManager::Register(UCameraComponent* Camera, UDisplayXRCamera* CameraRig)
{
	if (!Camera)
	{
		return;
	}

	// Avoid duplicates
	for (const auto& Weak : RegisteredCameras)
	{
		if (Weak.Get() == Camera)
		{
			return;
		}
	}

	RegisteredCameras.Add(Camera);

	// First registered becomes active
	if (!ActiveCamera.IsValid())
	{
		ActiveCamera = Camera;
	}
}

void FDisplayXRRigManager::Unregister(UCameraComponent* Camera)
{
	RegisteredCameras.RemoveAll([Camera](const TWeakObjectPtr<UCameraComponent>& Weak)
	{
		return !Weak.IsValid() || Weak.Get() == Camera;
	});

	if (ActiveCamera.Get() == Camera)
	{
		ActiveCamera = RegisteredCameras.Num() > 0 ? RegisteredCameras[0] : nullptr;
	}
}

UCameraComponent* FDisplayXRRigManager::GetActiveCamera()
{
	return ActiveCamera.Get();
}

void FDisplayXRRigManager::CycleNext()
{
	// Clean stale entries
	RegisteredCameras.RemoveAll([](const TWeakObjectPtr<UCameraComponent>& Weak)
	{
		return !Weak.IsValid();
	});

	if (RegisteredCameras.Num() <= 1)
	{
		return;
	}

	int32 CurrentIdx = -1;
	for (int32 i = 0; i < RegisteredCameras.Num(); i++)
	{
		if (RegisteredCameras[i].Get() == ActiveCamera.Get())
		{
			CurrentIdx = i;
			break;
		}
	}

	int32 NextIdx = (CurrentIdx + 1) % RegisteredCameras.Num();
	ActiveCamera = RegisteredCameras[NextIdx];
}

const TArray<TWeakObjectPtr<UCameraComponent>>& FDisplayXRRigManager::GetRegisteredCameras()
{
	return RegisteredCameras;
}
