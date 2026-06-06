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
			"DeveloperSettings",
			"Projects",
			"ImageCore",
			"ImageWrapper",
		});

		// D3D12 headers for swapchain GPU copy
		AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");

		// Bundled OpenXR headers + DisplayXR extension definitions
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "DisplayXRCore", "Private", "Native"));

		// DisplayXR stereo math helpers (DisplayXRStereoMath.h)
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "DisplayXRCore", "Private"));

		// Shared displayxr::math (Kooima view/projection) from the
		// displayxr-common submodule. The implementation is compiled into THIS
		// module via the Private/*_impl.c shims (DisplayXRCore has its own copy
		// but does not export it).
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "displayxr-common", "include"));
	}
}
