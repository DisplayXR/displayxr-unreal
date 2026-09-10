// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UCameraComponent;
class UDisplayXRCamera;
class UDisplayXRRigComponent;
class UWorld;

/**
 * Static registry of DisplayXR rig components and the single place tunables are
 * pushed from. Each frame PushActiveRig sends the tunables of the one rig whose
 * camera the local player renders from (the view target's first active camera,
 * AActor::CalcCamera's rule). Matches DisplayXRRigManager from the Unity plugin.
 */
class DISPLAYXRCORE_API FDisplayXRRigManager
{
public:
	/** Rig registry (called by UDisplayXRRigComponent on register/unregister). */
	static void RegisterRig(UDisplayXRRigComponent* Rig);
	static void UnregisterRig(UDisplayXRRigComponent* Rig);

	/** The camera the local player renders from in World, or null. */
	static UCameraComponent* FindViewCamera(const UWorld* World);

	/** Push the tunables of the rig on the view camera. Call once per frame before
	 *  the session locates views. Leaves the session untouched when no rig sits on
	 *  the view camera. */
	static void PushActiveRig(const UWorld* World);

	/** Register a camera with an optional camera-centric rig component. */
	static void Register(UCameraComponent* Camera, UDisplayXRCamera* CameraRig = nullptr);

	/** Unregister a camera. */
	static void Unregister(UCameraComponent* Camera);

	/** Get the active camera. First registered becomes active. */
	static UCameraComponent* GetActiveCamera();

	/** Cycle to the next registered camera. */
	static void CycleNext();

	/** Get all registered cameras. */
	static const TArray<TWeakObjectPtr<UCameraComponent>>& GetRegisteredCameras();

private:
	static TArray<TWeakObjectPtr<UDisplayXRRigComponent>> RegisteredRigs;
	static TArray<TWeakObjectPtr<UCameraComponent>> RegisteredCameras;
	static TWeakObjectPtr<UCameraComponent> ActiveCamera;
};
