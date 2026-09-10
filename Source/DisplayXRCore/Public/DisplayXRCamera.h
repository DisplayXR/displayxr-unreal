// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "DisplayXRRigComponent.h"
#include "DisplayXRCamera.generated.h"

/**
 * Camera-centric stereo rig for DisplayXR.
 *
 * Attach under the camera it drives (or anywhere on the same actor). The camera's
 * FOV and transform define the viewer; tunables control stereo separation and head
 * tracking. Only the rig on the camera the local player renders from is pushed
 * (see FDisplayXRRigManager).
 *
 * Mirrors DisplayXRCamera from the Unity plugin.
 */
UCLASS(ClassGroup = "DisplayXR", meta = (BlueprintSpawnableComponent, DisplayName = "DisplayXR Camera"))
class DISPLAYXRCORE_API UDisplayXRCamera : public UDisplayXRRigComponent
{
	GENERATED_BODY()

public:
	/** Scales inter-eye distance (0=mono, 1=full stereo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Camera", meta = (UIMin = "0.0", UIMax = "3.0"))
	float IpdFactor = 1.0f;

	/** Scales head-tracking parallax (0=no tracking, 1=full). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Camera", meta = (UIMin = "0.0", UIMax = "3.0"))
	float ParallaxFactor = 1.0f;

	/** 1/convergence distance (0=infinity). Higher values bring convergence closer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR|Camera", meta = (UIMin = "0.0", UIMax = "10.0"))
	float InvConvergenceDistance = 1.0f;

	virtual void BuildTunables(FDisplayXRTunables& OutTunables) override;

private:
	/** Cached initial FOV to prevent feedback loop with XR overrides. */
	float CachedFOV = 90.0f;
	bool bFOVCached = false;
};
