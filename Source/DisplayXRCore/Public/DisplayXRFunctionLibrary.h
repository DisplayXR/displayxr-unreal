// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DisplayXRTypes.h"
#include "DisplayXRFunctionLibrary.generated.h"

/**
 * Blueprint-exposed utility functions for DisplayXR.
 */
UCLASS()
class DISPLAYXRCORE_API UDisplayXRFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Get physical display properties from the runtime. */
	UFUNCTION(BlueprintPure, Category = "DisplayXR")
	static FDisplayXRDisplayInfo GetDisplayInfo();

	/** Get the current user/eye position (center of both eyes) in display space, in meters. */
	UFUNCTION(BlueprintPure, Category = "DisplayXR")
	static FVector GetUserPosition();

	/** Check if eye tracking is currently active. */
	UFUNCTION(BlueprintPure, Category = "DisplayXR")
	static bool IsEyeTracked();

	/** Request 2D or 3D display mode. Returns true if the request was accepted. */
	UFUNCTION(BlueprintCallable, Category = "DisplayXR")
	static bool RequestDisplayMode(bool bMode3D);

	/** Request eye tracking mode. true=MANUAL (app handles tracking loss), false=MANAGED (runtime handles it). */
	UFUNCTION(BlueprintCallable, Category = "DisplayXR")
	static bool RequestEyeTrackingMode(bool bManual);

	/** Cycle to the next registered DisplayXR rig. */
	UFUNCTION(BlueprintCallable, Category = "DisplayXR")
	static void CycleRig();

	/** Capture the current swapchain atlas to %USERPROFILE%\Pictures\DisplayXR\ as a PNG. */
	UFUNCTION(BlueprintCallable, Category = "DisplayXR")
	static void CaptureAtlasNow();
};
