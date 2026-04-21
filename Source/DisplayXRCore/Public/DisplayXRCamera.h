// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DisplayXRCamera.generated.h"

/**
 * Camera-centric stereo rig for DisplayXR.
 *
 * Attach to an actor with a UCameraComponent. The camera's FOV and transform
 * define the viewer position; tunables control stereo separation and head tracking.
 *
 * Mirrors DisplayXRCamera from the Unity plugin.
 */
UCLASS(ClassGroup = "DisplayXR", meta = (BlueprintSpawnableComponent, DisplayName = "DisplayXR Camera"))
class DISPLAYXRCORE_API UDisplayXRCamera : public UActorComponent
{
	GENERATED_BODY()

public:
	UDisplayXRCamera();

	/** Scales inter-eye distance (0=mono, 1=full stereo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Camera", meta = (UIMin = "0.0", UIMax = "3.0"))
	float IpdFactor = 1.0f;

	/** Scales head-tracking parallax (0=no tracking, 1=full). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Camera", meta = (UIMin = "0.0", UIMax = "3.0"))
	float ParallaxFactor = 1.0f;

	/** 1/convergence distance (0=infinity). Higher values bring convergence closer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Camera", meta = (UIMin = "0.0", UIMax = "10.0"))
	float InvConvergenceDistance = 1.0f;

	// UActorComponent interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	/** Find the camera component on the owning actor. */
	class UCameraComponent* GetCamera() const;

private:
	void PushTunables();

	/** Cached initial FOV to prevent feedback loop with XR overrides. */
	float CachedFOV = 90.0f;
	bool bFOVCached = false;
};
