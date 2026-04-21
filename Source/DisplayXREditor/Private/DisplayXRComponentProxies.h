// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "DisplayXRComponentProxies.generated.h"

/**
 * Editor visualization proxy for DisplayXR camera-centric rigs.
 * Renders wireframe frustum showing the stereo convergence plane and comfort zone.
 */
UCLASS(HideCategories = (Rendering, Physics, LOD, Collision), meta = (BlueprintSpawnableComponent))
class UDisplayXRCameraProxy : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UDisplayXRCameraProxy();

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};

/**
 * Editor visualization proxy for DisplayXR display-centric rigs.
 * Renders wireframe display surface rectangle and viewing cone.
 */
UCLASS(HideCategories = (Rendering, Physics, LOD, Collision), meta = (BlueprintSpawnableComponent))
class UDisplayXRDisplayProxy : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UDisplayXRDisplayProxy();

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};
