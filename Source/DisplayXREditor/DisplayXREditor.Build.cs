// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;
using System.IO;

public class DisplayXREditor : ModuleRules
{
	public DisplayXREditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// displayxr-common v2.0.0+ guards its layout assumptions with C11
		// _Static_assert. MSVC's default C mode (C89 + extensions) rejects it.
		CStandard = CStandardVersion.C17;

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
