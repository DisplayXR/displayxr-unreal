// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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

		// NOTE: no displayxr-common / displayxr::math include path — the runtime
		// owns the view math via XR_DXR_view_rig (#396 W7, ADR-024). Do not
		// re-add it; see the no-vendored-math drift guard.
	}
}
