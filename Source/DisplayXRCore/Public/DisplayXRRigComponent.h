// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DisplayXRTypes.h"
#include "DisplayXRRigComponent.generated.h"

class UCameraComponent;

/**
 * Base of the two stereo rigs. A rig is a scene component so it can be attached
 * under the camera it drives; FDisplayXRRigManager pushes the tunables of the one
 * rig whose camera the local player is rendering from.
 */
UCLASS(Abstract)
class DISPLAYXRCORE_API UDisplayXRRigComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UDisplayXRRigComponent();

	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	/** The camera this rig drives: the nearest UCameraComponent up the attach chain,
	 *  else the owner's first camera component. */
	UCameraComponent* GetCamera() const;

	/** Fill the tunables this rig wants pushed to the session. */
	virtual void BuildTunables(FDisplayXRTunables& OutTunables) {}
};
