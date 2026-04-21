// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "DisplayXRTypes.generated.h"

/**
 * Stereo rendering tunables pushed per-frame from rig components.
 */
USTRUCT(BlueprintType)
struct DISPLAYXRCORE_API FDisplayXRTunables
{
	GENERATED_BODY()

	/** Scales inter-eye distance (0=mono, 1=full stereo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "0.0", UIMax = "3.0"))
	float IpdFactor = 1.0f;

	/** Scales head-tracking parallax (0=no tracking, 1=full). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "0.0", UIMax = "3.0"))
	float ParallaxFactor = 1.0f;

	/** Scales eye depth for perspective distortion (display-centric only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "0.1", UIMax = "10.0"))
	float PerspectiveFactor = 1.0f;

	/** Virtual display height in world units (0=use physical). Display-centric only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "0.0"))
	float VirtualDisplayHeight = 0.0f;

	/** 1/convergence distance in meters (camera-centric only, 0=infinity). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "0.0"))
	float InvConvergenceDistance = 0.0f;

	/** FOV override in radians (0=compute from display, camera-centric only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR")
	float FovOverride = 0.0f;

	/** Near clip plane distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "0.01"))
	float NearZ = 0.1f;

	/** Far clip plane distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR", meta = (UIMin = "1.0"))
	float FarZ = 10000.0f;

	/** True if using camera-centric mode, false for display-centric. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DisplayXR")
	bool bCameraCentric = false;
};

/**
 * Display properties queried from the runtime via XR_EXT_display_info.
 */
USTRUCT(BlueprintType)
struct DISPLAYXRCORE_API FDisplayXRDisplayInfo
{
	GENERATED_BODY()

	/** Physical display width in meters. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	float DisplayWidthMeters = 0.0f;

	/** Physical display height in meters. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	float DisplayHeightMeters = 0.0f;

	/** Native display pixel width. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	int32 DisplayPixelWidth = 0;

	/** Native display pixel height. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	int32 DisplayPixelHeight = 0;

	/** Design-time viewer position in display space. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	FVector NominalViewerPosition = FVector::ZeroVector;

	/** Recommended horizontal render scale relative to display pixels. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	float RecommendedViewScaleX = 1.0f;

	/** Recommended vertical render scale relative to display pixels. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	float RecommendedViewScaleY = 1.0f;

	/** True if display info was successfully queried from the runtime. */
	UPROPERTY(BlueprintReadOnly, Category = "DisplayXR")
	bool bIsValid = false;
};
