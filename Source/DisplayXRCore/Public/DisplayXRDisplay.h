// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "DisplayXRRigComponent.h"
#include "DisplayXRDisplay.generated.h"

/**
 * Display-centric stereo rig for DisplayXR.
 *
 * Attach under the camera it drives (or anywhere on the same actor). The camera
 * transform IS the display plane and the viewer moves around it — as in the Unity
 * plugin, where DisplayXRDisplay sits on the Camera. The rig does not place a
 * display independently of the camera. Only the rig on the camera the local player
 * renders from is pushed (see FDisplayXRRigManager).
 *
 * Mirrors DisplayXRDisplay from the Unity plugin.
 */
UCLASS(ClassGroup = "DisplayXR", meta = (BlueprintSpawnableComponent, DisplayName = "DisplayXR Display"))
class DISPLAYXRCORE_API UDisplayXRDisplay : public UDisplayXRRigComponent
{
	GENERATED_BODY()

public:
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

	virtual void BuildTunables(FDisplayXRTunables& OutTunables) override;
};
