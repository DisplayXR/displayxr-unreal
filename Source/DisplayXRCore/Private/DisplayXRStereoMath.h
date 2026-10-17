// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include <openxr/openxr.h>
#include "Native/dxr_pose_math.h"

/**
 * Stereo math helpers for DisplayXR.
 *
 * The plugin computes no view math of its own. The DisplayXR runtime owns it
 * via XR_DXR_view_rig: we chain a rig descriptor onto xrLocateViews and get
 * back render-ready XrView{pose, fov} per eye. What remains here is strictly
 * convention work — OpenXR ↔ UE axis/unit conversion, and turning a
 * clip-independent XrFovf into UE's reverse-Z projection matrix.
 *
 * Do not add convergence-plane / eye-position frustum math back into this
 * file; the `no-vendored-math` workflow exists to keep it out.
 */

// ---------------------------------------------------------------------------
// OpenXR ↔ UE axis / unit conversion
//
// Thin FVector / FQuat wrappers over Native/dxr_pose_math.h, which holds the
// UE-free math (so Scripts/tests/pose_math_test.cpp can pin it with a bare
// compiler). The quaternion mapping matches Epic's OpenXRCore ToFQuat /
// ToXrQuat, sign on w included — the basis change is a reflection, so the
// conjugate (same components, +w) rotates the wrong way round.
// ---------------------------------------------------------------------------

/** OpenXR (m, X right / Y up / Z toward viewer) → UE (cm, X forward / Y right / Z up). */
static inline FVector OpenXRPositionToUE(const XrVector3f& V, float MetersToUnits = 100.0f)
{
	const dxr_pose::Vec3 P = dxr_pose::OpenXRPositionToUE(V, MetersToUnits);
	return FVector(P.x, P.y, P.z);
}

static inline FQuat OpenXROrientationToUE(const XrQuaternionf& Q)
{
	const dxr_pose::Quat R = dxr_pose::OpenXROrientationToUE(Q);
	return FQuat(R.x, R.y, R.z, R.w);
}

/** Full pose: OpenXR metres → UE units (default 100 per metre). */
static inline FTransform OpenXRPoseToUE(const XrPosef& P, float MetersToUnits = 100.0f)
{
	return FTransform(OpenXROrientationToUE(P.orientation), OpenXRPositionToUE(P.position, MetersToUnits));
}

static inline XrVector3f UEPositionToOpenXR(const FVector& V, float MetersToUnits = 100.0f)
{
	return dxr_pose::UEPositionToOpenXR(dxr_pose::Vec3{V.X, V.Y, V.Z}, MetersToUnits);
}

static inline XrQuaternionf UEOrientationToOpenXR(const FQuat& Q)
{
	return dxr_pose::UEOrientationToOpenXR(dxr_pose::Quat{Q.X, Q.Y, Q.Z, Q.W});
}

// ---------------------------------------------------------------------------
// Render-ready fov → UE projection
// ---------------------------------------------------------------------------

/**
 * Render-ready XrFovf → UE reverse-Z projection matrix.
 *
 * With XR_DXR_view_rig the runtime owns the view math and hands back an
 * asymmetric off-axis fov per view; we just convert the angles. The fov is
 * clip-independent by design, so near/far and the reverse-Z convention stay
 * ours.
 *
 * OpenXR gives signed half-angles (angleLeft/angleDown are negative), so the
 * tangents are the frustum extents at unit distance; scaling them by the near
 * plane cancels out of every term below, which is why GNearClippingPlane only
 * survives in the row-3 entry.
 */
static inline FMatrix ProjectionMatrixFromFov(const XrFovf& Fov)
{
	extern ENGINE_API float GNearClippingPlane;

	const float TanL = FMath::Tan(Fov.angleLeft);
	const float TanR = FMath::Tan(Fov.angleRight);
	const float TanU = FMath::Tan(Fov.angleUp);
	const float TanD = FMath::Tan(Fov.angleDown);

	const float WidthTan  = TanR - TanL;
	const float HeightTan = TanU - TanD;
	if (!(WidthTan > SMALL_NUMBER) || !(HeightTan > SMALL_NUMBER))
	{
		// Degenerate fov (runtime returned an empty/unset view) — a zero matrix
		// would black the frame, so fall back to identity and let the caller's
		// validity flag drive the decision.
		return FMatrix::Identity;
	}

	const float M00 = 2.0f / WidthTan;
	const float M11 = 2.0f / HeightTan;
	const float M20 = ((TanR + TanL) / WidthTan)  * -1.0f;
	const float M21 = ((TanU + TanD) / HeightTan) * -1.0f;

	return AdjustProjectionMatrixForRHI(FMatrix{
		FPlane(M00,  0.0f, 0.0f,               0.0f),
		FPlane(0.0f, M11,  0.0f,               0.0f),
		FPlane(M20,  M21,  0.0f,               1.0f),
		FPlane(0.0f, 0.0f, GNearClippingPlane, 0.0f),
	});
}
