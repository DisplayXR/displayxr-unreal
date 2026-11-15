// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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

	/**
	 * Midpoint of the two render-ready view positions the runtime located for
	 * the current frame (xrLocateViews with XR_DXR_view_rig). Relative to the
	 * active rig's origin (display rig: centre of the virtual display plane, i.e. the canvas;
	 * camera rig: the camera), in OpenXR axes (+X right, +Y up, +Z toward the
	 * viewer), not UE axes or centimetres. Metres for the camera rig; for the
	 * display rig, metres scaled by VirtualDisplayHeight / physical canvas
	 * height. The rig's IPD / parallax / perspective factors are already
	 * applied, so this is not the raw tracked head position. Zero until the
	 * first frame is located.
	 */
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
