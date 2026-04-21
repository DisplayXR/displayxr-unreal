// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"

class UCameraComponent;
class UDisplayXRCamera;

/**
 * Static registry of DisplayXR rig components.
 * Only the active rig pushes tunables to the extension plugin each frame.
 * Matches DisplayXRRigManager from the Unity plugin.
 */
class DISPLAYXRCORE_API FDisplayXRRigManager
{
public:
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
	static TArray<TWeakObjectPtr<UCameraComponent>> RegisteredCameras;
	static TWeakObjectPtr<UCameraComponent> ActiveCamera;
};
