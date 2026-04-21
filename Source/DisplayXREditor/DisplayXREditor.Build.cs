// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnrealBuildTool;
using System.IO;

public class DisplayXREditor : ModuleRules
{
	public DisplayXREditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"DisplayXRCore",
			"RHI",
			"RHICore",
			"D3D12RHI",
			"RenderCore",
			"Slate",
			"SlateCore",
			"LevelEditor",
			"HeadMountedDisplay",
			"Json",
			"JsonUtilities",
		});

		// D3D12 headers for swapchain GPU copy
		AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");

		// Bundled OpenXR headers + DisplayXR extension definitions
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "DisplayXRCore", "Private", "Native"));

		// DisplayXR stereo math helpers (DisplayXRStereoMath.h, display3d_view.h)
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "DisplayXRCore", "Private"));
	}
}
