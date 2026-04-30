// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnrealBuildTool;
using System.IO;

public class DisplayXRCore : ModuleRules
{
	public DisplayXRCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Allow .c files to compile as C (for Kooima math libraries)
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HeadMountedDisplay",
			"XRBase",
			"InputCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RHI",
			"RHICore",
			"D3D12RHI",
			"RenderCore",
			"Renderer",
			"Slate",
			"SlateCore",
			"Projects",
			"Json",
			"JsonUtilities",
			"ImageWrapper",
		});

		// D3D12 headers for compositor GPU copy
		AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");

		// All platforms use our bundled OpenXR headers (no UE OpenXR dependency)
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "Native"));
	}
}
