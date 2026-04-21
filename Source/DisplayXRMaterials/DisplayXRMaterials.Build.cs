// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnrealBuildTool;

public class DisplayXRMaterials : ModuleRules
{
	public DisplayXRMaterials(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});
	}
}
