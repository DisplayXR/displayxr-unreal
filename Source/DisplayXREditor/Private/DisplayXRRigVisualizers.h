// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "ComponentVisualizer.h"

/**
 * Draws the camera-centric rig's convergence plane while the component is selected:
 * the screen plane at 1/InvConvergenceDistance in front of the camera, sized from
 * the camera's FOV and aspect, with edges back to the camera. Parallel projection
 * (inverse distance 0) shows a 2 m preview plane in a different hue.
 */
class FDisplayXRCameraVisualizer : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
};

/**
 * Draws the display-centric rig's display plane while the component is selected, at
 * the camera transform (the camera IS the display plane), sized from
 * VirtualDisplayHeight or the physical display reported by the runtime.
 */
class FDisplayXRDisplayVisualizer : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
};
