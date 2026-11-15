// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/**
 * Unreal keys for controllers and styluses served by any DisplayXR input
 * provider (menu category "DisplayXR"). Bind them in an Enhanced Input
 * mapping context like any gamepad key; the analog ones are 1D axes in 0..1.
 *
 * Generic controller (khr/simple_controller, also bound on the stylus):
 *   Select  - simple: /input/select/click; stylus: the tip, thresholded by the runtime
 *   Menu    - simple: /input/menu/click
 * Stylus (MX Ink interaction profile):
 *   Tip_Force, Middle_Force            - axis 0..1
 *   Front_Click, Front_DoubleTap       - front cluster button
 *   Back_Click, Back_DoubleTap         - back cluster button
 *   Docked                             - in its dock
 *   System                             - /input/system/click (may be reserved by the runtime)
 */
struct DISPLAYXRINPUT_API FDisplayXRKeys
{
	static const FKey Left_Select;
	static const FKey Left_Menu;
	static const FKey Left_System;
	static const FKey Left_Tip_Force;
	static const FKey Left_Middle_Force;
	static const FKey Left_Front_Click;
	static const FKey Left_Front_DoubleTap;
	static const FKey Left_Back_Click;
	static const FKey Left_Back_DoubleTap;
	static const FKey Left_Docked;

	static const FKey Right_Select;
	static const FKey Right_Menu;
	static const FKey Right_System;
	static const FKey Right_Tip_Force;
	static const FKey Right_Middle_Force;
	static const FKey Right_Front_Click;
	static const FKey Right_Front_DoubleTap;
	static const FKey Right_Back_Click;
	static const FKey Right_Back_DoubleTap;
	static const FKey Right_Docked;

	/** The menu category every key above is listed under. */
	static const FName MenuCategory;
};

/**
 * Motion sources for UMotionControllerComponent::MotionSource (and
 * IMotionController queries). Poses are in DisplayXR tracking space — the rig
 * frame, camera at the origin — so put the component under the camera the
 * DisplayXR rig drives.
 *
 *   Left / Right            grip pose (Unreal's standard hand sources)
 *   LeftGrip / RightGrip    grip pose
 *   LeftAim / RightAim      aim pose
 *   LeftTip / RightTip      stylus tip pose; falls back to the aim pose on a
 *                           profile without a tip
 */
struct DISPLAYXRINPUT_API FDisplayXRMotionSources
{
	static const FName Left;
	static const FName Right;
	static const FName LeftGrip;
	static const FName RightGrip;
	static const FName LeftAim;
	static const FName RightAim;
	static const FName LeftTip;
	static const FName RightTip;
};
