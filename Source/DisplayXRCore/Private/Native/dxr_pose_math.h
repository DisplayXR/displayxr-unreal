// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Plain-C++ pose math shared by the plugin and the standalone test under
// Scripts/tests/. Deliberately UE-free (no FVector / FQuat) so it can be
// compiled and checked with a bare clang/gcc on any box, including one with
// no Unreal install. DisplayXRStereoMath.h wraps these into UE types.
//
// Conventions:
//  - OpenXR: metres, right-handed, +X right, +Y up, -Z forward (+Z toward the
//    viewer in display space). Quaternions are (x, y, z, w), Hamilton product,
//    rotating a vector as q * v * conj(q).
//  - UE: centimetres, left-handed, +X forward, +Y right, +Z up. Same
//    quaternion storage and product convention (FQuat::RotateVector is
//    q * v * conj(q)).
//
// The OpenXR -> UE basis change is a reflection (det = -1), so a rotation by
// angle t about axis a maps to a rotation by -t about the remapped axis: the
// mapped quaternion is (map(axis), -w), which is what Epic's OpenXRCore
// ToFQuat does: (-z, x, y, -w). Dropping the sign on w yields the conjugate,
// i.e. the rotation the wrong way round. Scripts/tests/pose_math_test.cpp
// pins this.

#pragma once

#include <openxr/openxr.h>

namespace dxr_pose
{

inline XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	XrQuaternionf r;
	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

inline XrQuaternionf QuatConj(const XrQuaternionf& q)
{
	return XrQuaternionf{-q.x, -q.y, -q.z, q.w};
}

/** q * v * conj(q). */
inline XrVector3f QuatRotate(const XrQuaternionf& q, const XrVector3f& v)
{
	const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
	const XrQuaternionf r = QuatMul(QuatMul(q, p), QuatConj(q));
	return XrVector3f{r.x, r.y, r.z};
}

/** a o b: apply b first, then a (a.pos + a.rot(b.pos), a.q * b.q). */
inline XrPosef PoseMul(const XrPosef& a, const XrPosef& b)
{
	XrPosef r;
	r.orientation = QuatMul(a.orientation, b.orientation);
	const XrVector3f t = QuatRotate(a.orientation, b.position);
	r.position = XrVector3f{a.position.x + t.x, a.position.y + t.y, a.position.z + t.z};
	return r;
}

inline XrPosef PoseInverse(const XrPosef& p)
{
	XrPosef r;
	r.orientation = QuatConj(p.orientation);
	const XrVector3f t = QuatRotate(r.orientation, p.position);
	r.position = XrVector3f{-t.x, -t.y, -t.z};
	return r;
}

inline XrPosef PoseIdentity()
{
	return XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

/** Rebase a pose given in a base space into the frame of `frame` (also given in
 *  that base space): inv(frame) o pose. This is how a LOCAL-space hand joint
 *  becomes display-plane-relative given XrViewDisplayRawDXR::displayPlanePose. */
inline XrPosef PoseInFrame(const XrPosef& frame, const XrPosef& pose)
{
	return PoseMul(PoseInverse(frame), pose);
}

// ---------------------------------------------------------------------------
// OpenXR <-> UE axis / unit conversion on raw floats. `scale` is the
// metres -> UE-units factor (100 for the engine default WorldToMeters).
// ---------------------------------------------------------------------------

struct Vec3 { double x, y, z; };
struct Quat { double x, y, z, w; };

/** OpenXR (m, X right / Y up / Z toward viewer) -> UE (cm, X forward / Y right / Z up). */
inline Vec3 OpenXRPositionToUE(const XrVector3f& v, double scale)
{
	return Vec3{-v.z * scale, v.x * scale, v.y * scale};
}

/** Matches Epic's OpenXRCore ToFQuat: (-z, x, y, -w). */
inline Quat OpenXROrientationToUE(const XrQuaternionf& q)
{
	return Quat{-q.z, q.x, q.y, -q.w};
}

inline XrVector3f UEPositionToOpenXR(const Vec3& v, double scale)
{
	return XrVector3f{
		static_cast<float>(v.y / scale),
		static_cast<float>(v.z / scale),
		static_cast<float>(-v.x / scale)};
}

/** Matches Epic's OpenXRCore ToXrQuat: (Y, Z, -X, -W). */
inline XrQuaternionf UEOrientationToOpenXR(const Quat& q)
{
	return XrQuaternionf{
		static_cast<float>(q.y),
		static_cast<float>(q.z),
		static_cast<float>(-q.x),
		static_cast<float>(-q.w)};
}

} // namespace dxr_pose
