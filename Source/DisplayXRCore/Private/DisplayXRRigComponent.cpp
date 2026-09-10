// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRRigComponent.h"
#include "DisplayXRRigManager.h"
#include "Camera/CameraComponent.h"

UDisplayXRRigComponent::UDisplayXRRigComponent()
{
	// Rigs do not tick; FDisplayXRRigManager pushes the active rig each frame.
	PrimaryComponentTick.bCanEverTick = false;
}

void UDisplayXRRigComponent::OnRegister()
{
	Super::OnRegister();
	FDisplayXRRigManager::RegisterRig(this);
}

void UDisplayXRRigComponent::OnUnregister()
{
	FDisplayXRRigManager::UnregisterRig(this);
	Super::OnUnregister();
}

UCameraComponent* UDisplayXRRigComponent::GetCamera() const
{
	for (USceneComponent* Parent = GetAttachParent(); Parent; Parent = Parent->GetAttachParent())
	{
		if (UCameraComponent* Camera = Cast<UCameraComponent>(Parent))
		{
			return Camera;
		}
	}
	AActor* Owner = GetOwner();
	return Owner ? Owner->FindComponentByClass<UCameraComponent>() : nullptr;
}
