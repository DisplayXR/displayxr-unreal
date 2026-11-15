// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "InputCoreTypes.h"
#include "DisplayXRInputFunctionLibrary.generated.h"

/**
 * Blueprint queries for DisplayXR controllers and styluses. Poses come through
 * UMotionControllerComponent (motion sources Left/Right, LeftAim/RightAim,
 * LeftTip/RightTip) and buttons through the DisplayXR keys; these cover what
 * those cannot say: which device is bound and the raw stylus forces.
 */
UCLASS()
class DISPLAYXRINPUT_API UDisplayXRInputFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** True while the DisplayXR session is up with its controller action set attached. */
	UFUNCTION(BlueprintPure, Category = "DisplayXR|Input")
	static bool IsDisplayXRControllerInputAvailable();

	/**
	 * What is bound to a hand. bActive: a device is bound and reporting;
	 * bIsStylus: it is served under the MX Ink stylus interaction profile;
	 * InteractionProfile: the OpenXR profile path, empty when none.
	 * Returns false for a hand other than Left / Right or when unavailable.
	 */
	UFUNCTION(BlueprintPure, Category = "DisplayXR|Input")
	static bool GetDisplayXRControllerInfo(EControllerHand Hand, bool& bActive, bool& bIsStylus, FString& InteractionProfile);

	/** Stylus tip and middle-cluster force, 0..1 (0 on a device without them). */
	UFUNCTION(BlueprintPure, Category = "DisplayXR|Input")
	static bool GetDisplayXRStylusForces(EControllerHand Hand, float& TipForce, float& MiddleForce);
};
