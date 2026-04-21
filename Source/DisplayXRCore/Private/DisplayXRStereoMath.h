// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include <openxr/openxr.h>

/**
 * Stereo math helpers for DisplayXR.
 *
 * We take raw eye positions from the DisplayXR OpenXR runtime's xrLocateViews
 * and feed them into a UE-native off-axis frustum pipeline. We do NOT consume
 * Kooima's projection_matrix[16] — that is OpenGL-convention and forcing it
 * into UE's pipeline fights UE's view construction / reverse-Z / LH axes.
 */

/** Per-frame per-eye output, consumed by the scene view extension. */
struct FDisplayXRStereoFrame
{
	FMatrix LeftProj = FMatrix::Identity;
	FMatrix RightProj = FMatrix::Identity;
	FVector LeftOffset = FVector::ZeroVector;   // UE cm, camera-local
	FVector RightOffset = FVector::ZeroVector;  // UE cm, camera-local
	bool bValid = false;
};

// ---------------------------------------------------------------------------
// OpenXR ↔ UE axis / unit conversion
// ---------------------------------------------------------------------------

/** OpenXR (m, X right / Y up / Z toward viewer) → UE (cm, X forward / Y right / Z up). */
static inline FVector OpenXRPositionToUE(const XrVector3f& V)
{
	return FVector(
		-V.z * 100.0f, // OpenXR +Z (toward viewer) → UE -X (backward)
		 V.x * 100.0f, // OpenXR +X (right)         → UE +Y (right)
		 V.y * 100.0f  // OpenXR +Y (up)            → UE +Z (up)
	);
}

static inline FQuat OpenXROrientationToUE(const XrQuaternionf& Q)
{
	return FQuat(-Q.z, Q.x, Q.y, Q.w);
}

static inline XrVector3f UEPositionToOpenXR(const FVector& V)
{
	XrVector3f Out;
	Out.x = static_cast<float>( V.Y * 0.01);
	Out.y = static_cast<float>( V.Z * 0.01);
	Out.z = static_cast<float>(-V.X * 0.01);
	return Out;
}

static inline XrQuaternionf UEOrientationToOpenXR(const FQuat& Q)
{
	XrQuaternionf Out;
	Out.x = static_cast<float>( Q.Y);
	Out.y = static_cast<float>( Q.Z);
	Out.z = static_cast<float>(-Q.X);
	Out.w = static_cast<float>( Q.W);
	return Out;
}

// ---------------------------------------------------------------------------
// UE-native off-axis math
// ---------------------------------------------------------------------------

/**
 * UE → screen-local axis permutation.
 *   Input:  UE (X-forward, Y-right, Z-up)
 *   Output: screen-local (x-right, y-up, z-out-of-screen-toward-viewer)
 */
static inline FVector ToScreenSpace(const FVector& V)
{
	return FVector(V.Y, V.Z, -V.X);
}

/**
 * UE-native asymmetric off-axis projection matrix.
 *
 * @param ViewportHalfSize  Half-extent (UE units) of the convergence plane
 * @param EyeLocation       Eye position in UE-local coords relative to screen
 *                          center. Caller passes X = -ConvergenceDistance, the
 *                          Y/Z from the scaled/factor-adjusted tracked eye.
 *
 * Produces a UE reverse-Z-compatible projection matrix:
 *   M[2][2]=0, M[2][3]=1, M[3][2]=GNearClippingPlane (infinite far plane).
 */
static inline FMatrix CalculateOffAxisProjectionMatrix(
	const FVector2D& ViewportHalfSize, const FVector& EyeLocation)
{
	extern ENGINE_API float GNearClippingPlane;

	const FVector EyeScreen = ToScreenSpace(EyeLocation);

	const FVector ScreenRight(1.f, 0.f, 0.f);
	const FVector ScreenUp(0.f, 1.f, 0.f);
	const FVector ScreenNormal(0.f, 0.f, 1.f);

	const FVector BottomLeft (-ViewportHalfSize.X, -ViewportHalfSize.Y, 0.f);
	const FVector BottomRight( ViewportHalfSize.X, -ViewportHalfSize.Y, 0.f);
	const FVector TopLeft    (-ViewportHalfSize.X,  ViewportHalfSize.Y, 0.f);

	const FVector BottomLeftToEye  = BottomLeft  - EyeScreen;
	const FVector BottomRightToEye = BottomRight - EyeScreen;
	const FVector TopLeftToEye     = TopLeft     - EyeScreen;

	const float EyeDistance = FVector::DotProduct(BottomLeftToEye, ScreenNormal) * -1.0f;
	const float InverseEyeDistanceNearPlane = GNearClippingPlane / EyeDistance;

	const float Left   = FVector::DotProduct(ScreenRight, BottomLeftToEye)  * InverseEyeDistanceNearPlane;
	const float Right  = FVector::DotProduct(ScreenRight, BottomRightToEye) * InverseEyeDistanceNearPlane;
	const float Bottom = FVector::DotProduct(ScreenUp,    BottomLeftToEye)  * InverseEyeDistanceNearPlane;
	const float Top    = FVector::DotProduct(ScreenUp,    TopLeftToEye)     * InverseEyeDistanceNearPlane;

	const float M00 = 2.0f * GNearClippingPlane / (Right - Left);
	const float M11 = 2.0f * GNearClippingPlane / (Top - Bottom);
	const float M20 = ((Right + Left)   / (Right - Left))   * -1.0f;
	const float M21 = ((Top   + Bottom) / (Top   - Bottom)) * -1.0f;

	return AdjustProjectionMatrixForRHI(FMatrix{
		FPlane(M00,  0.0f, 0.0f,               0.0f),
		FPlane(0.0f, M11,  0.0f,               0.0f),
		FPlane(M20,  M21,  0.0f,               1.0f),
		FPlane(0.0f, 0.0f, GNearClippingPlane, 0.0f),
	});
}

/**
 * Raw tracked eyes → lookaround/baseline-adjusted per-eye offsets + center.
 * Caller passes eyes already in UE units (handles Scale upstream).
 *
 * All inputs/outputs in UE local coords (X-forward, Y-right, Z-up), UE units.
 */
static inline void EyesToOffsets(
	float LookaroundFactor, float BaselineFactor,
	const FVector& InLeft, const FVector& InRight,
	FVector& OutLeft, FVector& OutRight, FVector& OutCenter)
{
	const float ClampedLookaround = FMath::Max(LookaroundFactor, 0.0f);
	const float ClampedBaseline   = FMath::Max(BaselineFactor,   0.0f);

	OutLeft  = InLeft;
	OutRight = InRight;

	OutCenter = (OutLeft + OutRight) * 0.5f;
	OutCenter.X = 0.0f;
	OutCenter = FMath::Lerp(FVector::ZeroVector, OutCenter, ClampedLookaround);

	const FVector HalfEyeOffset = (OutRight - OutLeft) * 0.5f;
	OutLeft  = OutCenter - HalfEyeOffset * ClampedBaseline;
	OutRight = OutCenter + HalfEyeOffset * ClampedBaseline;
}
