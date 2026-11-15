// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "CoreMinimal.h"
#include "IInputDeviceModule.h"
#include "Modules/ModuleManager.h"
#include "DisplayXRInputDevice.h"
#include "DisplayXRInputKeys.h"

#define LOCTEXT_NAMESPACE "DisplayXRInput"

/**
 * DisplayXRInput: controllers and styluses from any DisplayXR input provider
 * as Unreal input (keys, motion-controller sources, haptics). Registers the
 * FDisplayXRKeys at startup and hands the engine an FDisplayXRInputDevice
 * through IInputDeviceModule. The OpenXR action set itself lives in
 * DisplayXRCore, which owns the session.
 */
class FDisplayXRInputModule : public IInputDeviceModule
{
public:
	virtual void StartupModule() override
	{
		IInputDeviceModule::StartupModule(); // registers the "InputDevice" modular feature
		RegisterKeys();
	}

	virtual void ShutdownModule() override
	{
		IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), this);
	}

	virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override
	{
		return MakeShared<FDisplayXRInputDevice>(InMessageHandler);
	}

private:
	static void RegisterKeys()
	{
		const FName Category = FDisplayXRKeys::MenuCategory;
		EKeys::AddMenuCategoryDisplayInfo(Category, LOCTEXT("DisplayXRCategory", "DisplayXR"), TEXT("GraphEditor.PadEvent_16x"));

		const uint32 Button = FKeyDetails::GamepadKey;
		const uint32 Axis = FKeyDetails::GamepadKey | FKeyDetails::Axis1D;

		struct FKeyDef { const FKey& Key; FText Name; uint32 Flags; };
		const FKeyDef Defs[] = {
			{FDisplayXRKeys::Left_Select, LOCTEXT("L_Select", "DisplayXR (L) Select"), Button},
			{FDisplayXRKeys::Left_Menu, LOCTEXT("L_Menu", "DisplayXR (L) Menu"), Button},
			{FDisplayXRKeys::Left_System, LOCTEXT("L_System", "DisplayXR (L) System"), Button},
			{FDisplayXRKeys::Left_Tip_Force, LOCTEXT("L_TipForce", "DisplayXR (L) Stylus Tip Force"), Axis},
			{FDisplayXRKeys::Left_Middle_Force, LOCTEXT("L_MiddleForce", "DisplayXR (L) Stylus Middle Force"), Axis},
			{FDisplayXRKeys::Left_Front_Click, LOCTEXT("L_FrontClick", "DisplayXR (L) Stylus Front Button"), Button},
			{FDisplayXRKeys::Left_Front_DoubleTap, LOCTEXT("L_FrontDoubleTap", "DisplayXR (L) Stylus Front Double Tap"), Button},
			{FDisplayXRKeys::Left_Back_Click, LOCTEXT("L_BackClick", "DisplayXR (L) Stylus Back Button"), Button},
			{FDisplayXRKeys::Left_Back_DoubleTap, LOCTEXT("L_BackDoubleTap", "DisplayXR (L) Stylus Back Double Tap"), Button},
			{FDisplayXRKeys::Left_Docked, LOCTEXT("L_Docked", "DisplayXR (L) Stylus Docked"), Button},
			{FDisplayXRKeys::Right_Select, LOCTEXT("R_Select", "DisplayXR (R) Select"), Button},
			{FDisplayXRKeys::Right_Menu, LOCTEXT("R_Menu", "DisplayXR (R) Menu"), Button},
			{FDisplayXRKeys::Right_System, LOCTEXT("R_System", "DisplayXR (R) System"), Button},
			{FDisplayXRKeys::Right_Tip_Force, LOCTEXT("R_TipForce", "DisplayXR (R) Stylus Tip Force"), Axis},
			{FDisplayXRKeys::Right_Middle_Force, LOCTEXT("R_MiddleForce", "DisplayXR (R) Stylus Middle Force"), Axis},
			{FDisplayXRKeys::Right_Front_Click, LOCTEXT("R_FrontClick", "DisplayXR (R) Stylus Front Button"), Button},
			{FDisplayXRKeys::Right_Front_DoubleTap, LOCTEXT("R_FrontDoubleTap", "DisplayXR (R) Stylus Front Double Tap"), Button},
			{FDisplayXRKeys::Right_Back_Click, LOCTEXT("R_BackClick", "DisplayXR (R) Stylus Back Button"), Button},
			{FDisplayXRKeys::Right_Back_DoubleTap, LOCTEXT("R_BackDoubleTap", "DisplayXR (R) Stylus Back Double Tap"), Button},
			{FDisplayXRKeys::Right_Docked, LOCTEXT("R_Docked", "DisplayXR (R) Stylus Docked"), Button},
		};
		for (const FKeyDef& Def : Defs)
		{
			// A hot-reload / module re-load must not add a key twice.
			if (!EKeys::GetKeyDetails(Def.Key).IsValid())
			{
				EKeys::AddKey(FKeyDetails(Def.Key, Def.Name, Def.Flags, Category));
			}
		}
	}
};

IMPLEMENT_MODULE(FDisplayXRInputModule, DisplayXRInput)

#undef LOCTEXT_NAMESPACE
