// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRRigManager.h"
#include "Camera/CameraComponent.h"

TArray<TWeakObjectPtr<UCameraComponent>> FDisplayXRRigManager::RegisteredCameras;
TWeakObjectPtr<UCameraComponent> FDisplayXRRigManager::ActiveCamera;

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
