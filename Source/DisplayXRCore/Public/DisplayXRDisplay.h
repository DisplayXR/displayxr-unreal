// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DisplayXRDisplay.generated.h"

/**
 * Display-centric stereo rig for DisplayXR.
 *
 * Attach to an actor representing the virtual display surface. The parent
 * transform defines the display position/orientation; the camera is a child.
 *
 * Mirrors DisplayXRDisplay from the Unity plugin.
 */
UCLASS(ClassGroup = "DisplayXR", meta = (BlueprintSpawnableComponent, DisplayName = "DisplayXR Display"))
class DISPLAYXRCORE_API UDisplayXRDisplay : public UActorComponent
{
	GENERATED_BODY()

public:
	UDisplayXRDisplay();

	/** Scales inter-eye distance (0=mono, 1=full stereo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Display", meta = (UIMin = "0.0", UIMax = "3.0"))
	float IpdFactor = 1.0f;

	/** Scales head-tracking parallax (0=no tracking, 1=full). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Display", meta = (UIMin = "0.0", UIMax = "3.0"))
	float ParallaxFactor = 1.0f;

	/** Scales eye depth for perspective distortion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Display", meta = (UIMin = "0.1", UIMax = "10.0"))
	float PerspectiveFactor = 1.0f;

	/** Virtual display height in world units (0=use physical display dimensions). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Display", meta = (UIMin = "0.0"))
	float VirtualDisplayHeight = 0.0f;

	// UActorComponent interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	/** Find the camera component on the owning actor or its children. */
	class UCameraComponent* GetCamera() const;

private:
	void PushTunables();
};
