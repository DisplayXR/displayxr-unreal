// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// Standalone check of Source/DisplayXRCore/Private/Native/dxr_pose_math.h —
// the OpenXR <-> UE conversion and the XrPosef helpers the hand-tracking path
// relies on. Needs no Unreal install; build and run with a bare compiler
// (one line; a trailing backslash in a // comment is itself a GCC warning):
//
//   clang++ -std=c++17 -I Source/DisplayXRCore/Private/Native Scripts/tests/pose_math_test.cpp -o /tmp/pose_math_test && /tmp/pose_math_test
//
// The lint workflow runs exactly this (g++ -Wall -Wextra -Werror) on every PR.

#include "dxr_pose_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dxr_pose;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
	if (!ok)
	{
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

static bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

static XrQuaternionf AxisAngle(float ax, float ay, float az, float angle)
{
	const float n = std::sqrt(ax * ax + ay * ay + az * az);
	const float s = std::sin(angle * 0.5f) / n;
	return XrQuaternionf{ax * s, ay * s, az * s, std::cos(angle * 0.5f)};
}

// UE-side Hamilton rotate on the plain structs (same convention as FQuat::RotateVector).
static Vec3 UERotate(const Quat& q, const Vec3& v)
{
	const XrQuaternionf qf{(float)q.x, (float)q.y, (float)q.z, (float)q.w};
	const XrVector3f vf{(float)v.x, (float)v.y, (float)v.z};
	const XrVector3f r = QuatRotate(qf, vf);
	return Vec3{r.x, r.y, r.z};
}

// Deterministic pseudo-random in [-1, 1].
static float Rnd(unsigned& s)
{
	s = s * 1664525u + 1013904223u;
	return ((s >> 8) & 0xffff) / 32767.5f - 1.0f;
}

int main()
{
	unsigned seed = 12345u;

	// 1. The orientation mapping must commute with rotation under the basis
	//    change: map(q * v * q^-1) == map(q) * map(v) * map(q)^-1. This is the
	//    property that fails for (-z, x, y, +w) (the old plugin helper), which
	//    is the conjugate and rotates the wrong way round.
	for (int i = 0; i < 200; ++i)
	{
		const XrQuaternionf q = AxisAngle(Rnd(seed), Rnd(seed), Rnd(seed), Rnd(seed) * 3.0f);
		const XrVector3f v{Rnd(seed), Rnd(seed), Rnd(seed)};
		const XrVector3f rv = QuatRotate(q, v);

		const Vec3 lhs = OpenXRPositionToUE(rv, 100.0);
		const Vec3 rhs = UERotate(OpenXROrientationToUE(q), OpenXRPositionToUE(v, 100.0));
		Check(Near(lhs.x, rhs.x, 1e-3) && Near(lhs.y, rhs.y, 1e-3) && Near(lhs.z, rhs.z, 1e-3),
			"OpenXROrientationToUE commutes with rotation (Epic ToFQuat sign on w)");

		// The old helper, kept here as a negative control so the test is known
		// to discriminate: it must fail for a generic rotation.
		const Quat wrong{-q.z, q.x, q.y, q.w};
		const Vec3 bad = UERotate(wrong, OpenXRPositionToUE(v, 100.0));
		if (i == 0)
		{
			Check(!(Near(lhs.x, bad.x, 1e-3) && Near(lhs.y, bad.y, 1e-3) && Near(lhs.z, bad.z, 1e-3)),
				"negative control: the conjugate mapping (-z, x, y, +w) is NOT equivalent");
		}
	}

	// 2. Axis spot checks: OpenXR +X (right) -> UE +Y; OpenXR +Y (up) -> UE +Z;
	//    OpenXR -Z (forward) -> UE +X. All at 100 units per metre.
	{
		const Vec3 r = OpenXRPositionToUE(XrVector3f{1, 0, 0}, 100.0);
		const Vec3 u = OpenXRPositionToUE(XrVector3f{0, 1, 0}, 100.0);
		const Vec3 f = OpenXRPositionToUE(XrVector3f{0, 0, -1}, 100.0);
		Check(Near(r.x, 0) && Near(r.y, 100) && Near(r.z, 0), "OpenXR +X -> UE +Y");
		Check(Near(u.x, 0) && Near(u.y, 0) && Near(u.z, 100), "OpenXR +Y -> UE +Z");
		Check(Near(f.x, 100) && Near(f.y, 0) && Near(f.z, 0), "OpenXR -Z -> UE +X");
	}

	// 3. A 90-degree yaw about OpenXR +Y (up) must come out as a yaw about UE +Z
	//    that turns UE +X (forward) into UE -Y (left), because a positive
	//    rotation about +Y in a right-handed frame turns -Z (forward) toward -X
	//    (left). This is the handedness case the sign on w decides.
	{
		const XrQuaternionf yaw = AxisAngle(0, 1, 0, 3.14159265f * 0.5f);
		const Quat ue = OpenXROrientationToUE(yaw);
		const Vec3 fwd = UERotate(ue, Vec3{100, 0, 0});
		Check(Near(fwd.x, 0, 1e-3) && Near(fwd.y, -100, 1e-3) && Near(fwd.z, 0, 1e-3),
			"OpenXR +90deg yaw turns UE forward to UE left");
	}

	// 4. Round trips.
	for (int i = 0; i < 50; ++i)
	{
		const XrQuaternionf q = AxisAngle(Rnd(seed), Rnd(seed), Rnd(seed), Rnd(seed) * 3.0f);
		const XrVector3f v{Rnd(seed), Rnd(seed), Rnd(seed)};
		const XrVector3f v2 = UEPositionToOpenXR(OpenXRPositionToUE(v, 100.0), 100.0);
		const XrQuaternionf q2 = UEOrientationToOpenXR(OpenXROrientationToUE(q));
		Check(Near(v.x, v2.x) && Near(v.y, v2.y) && Near(v.z, v2.z), "position round trip");
		Check(Near(q.x, q2.x) && Near(q.y, q2.y) && Near(q.z, q2.z) && Near(q.w, q2.w), "orientation round trip");
	}

	// 5. Pose algebra: p o inv(p) == identity, and (a o b)(v) == a(b(v)).
	for (int i = 0; i < 50; ++i)
	{
		XrPosef a{AxisAngle(Rnd(seed), Rnd(seed), Rnd(seed), Rnd(seed) * 3.0f), {Rnd(seed), Rnd(seed), Rnd(seed)}};
		XrPosef b{AxisAngle(Rnd(seed), Rnd(seed), Rnd(seed), Rnd(seed) * 3.0f), {Rnd(seed), Rnd(seed), Rnd(seed)}};
		const XrPosef id = PoseMul(a, PoseInverse(a));
		Check(Near(id.position.x, 0) && Near(id.position.y, 0) && Near(id.position.z, 0)
			&& Near(std::fabs(id.orientation.w), 1) && Near(id.orientation.x, 0)
			&& Near(id.orientation.y, 0) && Near(id.orientation.z, 0), "p o inv(p) == identity");

		const XrVector3f v{Rnd(seed), Rnd(seed), Rnd(seed)};
		const XrPosef ab = PoseMul(a, b);
		const XrVector3f lhs = QuatRotate(ab.orientation, v);
		const XrVector3f l{ab.position.x + lhs.x, ab.position.y + lhs.y, ab.position.z + lhs.z};
		const XrVector3f bv = QuatRotate(b.orientation, v);
		const XrVector3f bv2{b.position.x + bv.x, b.position.y + bv.y, b.position.z + bv.z};
		const XrVector3f abv = QuatRotate(a.orientation, bv2);
		const XrVector3f r{a.position.x + abv.x, a.position.y + abv.y, a.position.z + abv.z};
		Check(Near(l.x, r.x) && Near(l.y, r.y) && Near(l.z, r.z), "(a o b)(v) == a(b(v))");
	}

	// 6. Display-plane rebase, the hand-tracking case: the runtime reports the
	//    display plane at (0, 1.2, -0.6) in LOCAL (a seated viewer 0.6 m in
	//    front of a panel whose centre is 1.2 m up). A joint 0.3 m in front of
	//    the panel centre is at LOCAL (0, 1.2, -0.3); in display-plane space it
	//    must be (0, 0, +0.3) — +Z toward the viewer.
	{
		const XrPosef plane{{0, 0, 0, 1}, {0.0f, 1.2f, -0.6f}};
		const XrPosef joint{{0, 0, 0, 1}, {0.0f, 1.2f, -0.3f}};
		const XrPosef inPlane = PoseInFrame(plane, joint);
		Check(Near(inPlane.position.x, 0) && Near(inPlane.position.y, 0) && Near(inPlane.position.z, 0.3f),
			"LOCAL joint rebased into display-plane space");
		// And a rotated plane: yaw 90deg about +Y with the joint 0.3 m along the
		// plane's own +Z (which now points along LOCAL +X).
		const XrPosef plane2{AxisAngle(0, 1, 0, 3.14159265f * 0.5f), {0.0f, 1.2f, -0.6f}};
		const XrPosef joint2{{0, 0, 0, 1}, {0.3f, 1.2f, -0.6f}};
		const XrPosef inPlane2 = PoseInFrame(plane2, joint2);
		Check(Near(inPlane2.position.x, 0, 1e-4) && Near(inPlane2.position.y, 0, 1e-4) && Near(inPlane2.position.z, 0.3f, 1e-4),
			"LOCAL joint rebased into a yawed display-plane space");
	}

	if (g_failures == 0)
	{
		std::printf("pose_math_test: all checks passed\n");
		return 0;
	}
	std::fprintf(stderr, "pose_math_test: %d check(s) FAILED\n", g_failures);
	return 1;
}
