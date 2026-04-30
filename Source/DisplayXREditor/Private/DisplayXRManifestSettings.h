// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/Texture2D.h"
#include "DisplayXRManifestSettings.generated.h"

UENUM()
enum class EDisplayXRManifestCategory : uint8
{
	App     UMETA(DisplayName = "App"),
	Demo    UMETA(DisplayName = "Demo"),
	Test    UMETA(DisplayName = "Test"),
	Tool    UMETA(DisplayName = "Tool"),
};

UENUM()
enum class EDisplayXRStereoLayout : uint8
{
	SbsLr   UMETA(DisplayName = "Side-by-Side (Left|Right)"),
	SbsRl   UMETA(DisplayName = "Side-by-Side (Right|Left)"),
	Tb      UMETA(DisplayName = "Top|Bottom"),
	Bt      UMETA(DisplayName = "Bottom|Top"),
};

/**
 * Editor-only settings that describe an Unreal-built app to DisplayXR-compatible
 * workspace controllers (the DisplayXR Shell is the reference; third-party
 * verticals/kiosks/OEM-branded controllers may also play this role).
 *
 * On change, the editor writes the resolved values to
 * <Project>/Saved/Config/DisplayXRManifest.json, exporting any Icon textures to
 * PNGs alongside it. Scripts/PackageApp.py reads that file post-stage and emits
 * the launcher-discoverable manifest(s).
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "DisplayXR Manifest"))
class UDisplayXRManifestSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UDisplayXRManifestSettings();

	/** Display name shown on the launcher tile. Leave empty to use the project name. */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest")
	FString AppName;

	/** Free-form category tag for the launcher. */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest")
	EDisplayXRManifestCategory Category = EDisplayXRManifestCategory::App;

	/** Preferred display rendering mode at launch. */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest")
	FString DisplayMode = TEXT("auto");

	/** One-line description for tooltips (max 256 chars). */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest", meta = (MultiLine = true))
	FString Description;

	/** 2D tile icon (PNG, 512x512 recommended). Leave empty for text-only tile. */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest", meta = (AllowedClasses = "/Script/Engine.Texture2D"))
	TSoftObjectPtr<UTexture2D> Icon;

	/** Stereoscopic tile icon (1024x512 recommended for sbs). Enables 3D tile in launcher. */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest", meta = (AllowedClasses = "/Script/Engine.Texture2D"))
	TSoftObjectPtr<UTexture2D> Icon3D;

	/** How the stereo pair is packed in the 3D icon. */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest")
	EDisplayXRStereoLayout Icon3DLayout = EDisplayXRStereoLayout::SbsLr;

	/**
	 * When ON, the staging script also writes a registered manifest to
	 * %LOCALAPPDATA%\DisplayXR\apps\ so DisplayXR-compatible workspace controllers
	 * (including the DisplayXR Shell) discover this build without it living under
	 * Program Files.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "DisplayXR Manifest")
	bool bRegisterWithDisplayXR = false;

	/** Manual export trigger — re-writes the resolved manifest JSON and icon PNGs. */
	UFUNCTION(CallInEditor, Category = "DisplayXR Manifest", meta = (DisplayName = "Export Manifest Now"))
	void ExportManifestNow();

	/** Shared write path. bShowToast=true surfaces the icon-mismatch warning as a Slate toast. */
	void WriteResolvedManifest(bool bShowToast);

	//~ UDeveloperSettings
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Resolves AppName, falling back to the UE project name when empty. */
	FString GetEffectiveName() const;

	/** Where the editor writes the resolved manifest for the staging script to read. */
	static FString GetResolvedManifestPath();

	/** Subdir (sibling to the resolved manifest) for exported icon PNGs. */
	static FString GetExportedIconDir();
};
