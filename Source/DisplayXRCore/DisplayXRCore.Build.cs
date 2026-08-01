// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;
using System.IO;

public class DisplayXRCore : ModuleRules
{
	public DisplayXRCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Allow .c files to compile as C (for Kooima math libraries)
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;

		// displayxr-common v2.0.0+ guards its layout assumptions with C11
		// _Static_assert. MSVC's default C mode (C89 + extensions) rejects it.
		CStandard = CStandardVersion.C17;

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

		// Shared displayxr::math (Kooima view/projection) headers from the
		// displayxr-common submodule. The implementation is compiled into this
		// module via the Private/Native/*_impl.c shims.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "displayxr-common", "include"));
	}
}
