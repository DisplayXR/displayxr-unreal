// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class DisplayXRInput : ModuleRules
{
	public DisplayXRInput(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"HeadMountedDisplay",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"InputDevice",
			"XRBase",
			// FDisplayXRControllerAPI: the core module owns the OpenXR session
			// and its action set; this module only maps it onto Unreal input.
			"DisplayXRCore",
		});
	}
}
