// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRDisplay.h"

void UDisplayXRDisplay::BuildTunables(FDisplayXRTunables& T)
{
	T.IpdFactor = IpdFactor;
	T.ParallaxFactor = ParallaxFactor;
	T.PerspectiveFactor = PerspectiveFactor;
	T.VirtualDisplayHeight = VirtualDisplayHeight;
	T.NearZ = 0.1f;
	T.FarZ = 10000.0f;
	T.bCameraCentric = false;
}
