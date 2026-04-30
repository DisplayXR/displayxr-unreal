// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRManifestSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Internationalization/Text.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPtr.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRManifest, Log, All);

namespace DisplayXRManifest
{
	static const TCHAR* CategoryToString(EDisplayXRManifestCategory C)
	{
		switch (C)
		{
		case EDisplayXRManifestCategory::Demo: return TEXT("demo");
		case EDisplayXRManifestCategory::Test: return TEXT("test");
		case EDisplayXRManifestCategory::Tool: return TEXT("tool");
		case EDisplayXRManifestCategory::App:
		default:                               return TEXT("app");
		}
	}

	static const TCHAR* LayoutToString(EDisplayXRStereoLayout L)
	{
		switch (L)
		{
		case EDisplayXRStereoLayout::SbsRl: return TEXT("sbs-rl");
		case EDisplayXRStereoLayout::Tb:    return TEXT("tb");
		case EDisplayXRStereoLayout::Bt:    return TEXT("bt");
		case EDisplayXRStereoLayout::SbsLr:
		default:                            return TEXT("sbs-lr");
		}
	}

	static bool ExportTextureToPng(UTexture2D* Texture, const FString& OutPath)
	{
		if (!Texture)
		{
			return false;
		}

		FImage SourceImage;
		if (!FImageUtils::GetTexture2DSourceImage(Texture, SourceImage))
		{
			UE_LOG(LogDisplayXRManifest, Warning, TEXT("Failed to read source image for texture '%s'"), *Texture->GetName());
			return false;
		}

		TArray64<uint8> CompressedBytes;
		if (!FImageUtils::CompressImage(CompressedBytes, TEXT("png"), SourceImage))
		{
			UE_LOG(LogDisplayXRManifest, Warning, TEXT("Failed to PNG-compress texture '%s'"), *Texture->GetName());
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
		if (!FFileHelper::SaveArrayToFile(CompressedBytes, *OutPath))
		{
			UE_LOG(LogDisplayXRManifest, Warning, TEXT("Failed to write PNG to '%s'"), *OutPath);
			return false;
		}
		return true;
	}

	static void ShowToast(const FText& Message, bool bWarning)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bWarning ? 8.0f : 4.0f;
		Info.bFireAndForget = true;
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bWarning ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
		}
	}
}

UDisplayXRManifestSettings::UDisplayXRManifestSettings()
{
	CategoryName = FName(TEXT("Plugins"));
	SectionName = FName(TEXT("DisplayXRManifest"));
}

#if WITH_EDITOR
FText UDisplayXRManifestSettings::GetSectionText() const
{
	return NSLOCTEXT("DisplayXR", "DisplayXRManifestSection", "DisplayXR Manifest");
}
#endif

FString UDisplayXRManifestSettings::GetEffectiveName() const
{
	const FString Trimmed = AppName.TrimStartAndEnd();
	return Trimmed.IsEmpty() ? FString(FApp::GetProjectName()) : Trimmed;
}

FString UDisplayXRManifestSettings::GetResolvedManifestPath()
{
	return FPaths::ProjectSavedDir() / TEXT("Config") / TEXT("DisplayXRManifest.json");
}

FString UDisplayXRManifestSettings::GetExportedIconDir()
{
	return FPaths::ProjectSavedDir() / TEXT("Config") / TEXT("DisplayXRManifest");
}

void UDisplayXRManifestSettings::ExportManifestNow()
{
	WriteResolvedManifest(/*bShowToast=*/true);
}

void UDisplayXRManifestSettings::WriteResolvedManifest(bool bShowToast)
{
	using namespace DisplayXRManifest;

	const FString IconDir = GetExportedIconDir();
	IFileManager::Get().MakeDirectory(*IconDir, /*Tree=*/true);

	FString IconPath;
	FString Icon3DPath;

	if (UTexture2D* IconTex = Icon.LoadSynchronous())
	{
		const FString Out = IconDir / TEXT("icon.png");
		if (ExportTextureToPng(IconTex, Out))
		{
			IconPath = Out;
		}
	}
	if (UTexture2D* Icon3DTex = Icon3D.LoadSynchronous())
	{
		const FString Out = IconDir / TEXT("icon_3d.png");
		if (ExportTextureToPng(Icon3DTex, Out))
		{
			Icon3DPath = Out;
		}
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("name"), GetEffectiveName());
	Root->SetStringField(TEXT("type"), TEXT("3d"));
	Root->SetStringField(TEXT("category"), CategoryToString(Category));
	Root->SetStringField(TEXT("display_mode"), DisplayMode);

	FString Desc = Description;
	if (Desc.Len() > 256)
	{
		Desc = Desc.Left(256);
	}
	Root->SetStringField(TEXT("description"), Desc);

	if (!IconPath.IsEmpty())
	{
		Root->SetStringField(TEXT("icon_source"), IconPath);
	}
	if (!Icon3DPath.IsEmpty())
	{
		Root->SetStringField(TEXT("icon_3d_source"), Icon3DPath);
	}
	Root->SetStringField(TEXT("icon_3d_layout"), LayoutToString(Icon3DLayout));
	Root->SetBoolField(TEXT("register_with_displayxr"), bRegisterWithDisplayXR);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);

	const FString OutPath = GetResolvedManifestPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(Out, *OutPath))
	{
		UE_LOG(LogDisplayXRManifest, Error, TEXT("Failed to write resolved manifest to '%s'"), *OutPath);
		return;
	}
	UE_LOG(LogDisplayXRManifest, Log, TEXT("Wrote DisplayXR manifest: %s"), *OutPath);

	// Icon-mismatch warning: keep the launcher tile icon and the Windows
	// Start-Menu / taskbar icon in sync. The Windows .ico that UE embeds in the
	// PE is conventionally at <Project>/Build/Windows/Application.ico (set via
	// Project Settings -> Platforms -> Windows -> Game Icon).
	const bool bManifestIconSet = !Icon.IsNull();
	const FString WindowsIcoPath = FPaths::ProjectDir() / TEXT("Build") / TEXT("Windows") / TEXT("Application.ico");
	const bool bWindowsIconSet = FPaths::FileExists(WindowsIcoPath);
	if (bManifestIconSet != bWindowsIconSet)
	{
		const FText Msg = bManifestIconSet
			? NSLOCTEXT("DisplayXR", "IconMismatchWindowsMissing",
				"DisplayXR Manifest Icon is set, but Project Settings -> Platforms -> Windows -> Game Icon is not. Consider sharing one source asset.")
			: NSLOCTEXT("DisplayXR", "IconMismatchManifestMissing",
				"Project Settings -> Platforms -> Windows -> Game Icon is set, but DisplayXR Manifest Icon is not. Consider sharing one source asset.");
		UE_LOG(LogDisplayXRManifest, Warning, TEXT("%s"), *Msg.ToString());
		if (bShowToast)
		{
			ShowToast(Msg, /*bWarning=*/true);
		}
	}
}

#if WITH_EDITOR
void UDisplayXRManifestSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	WriteResolvedManifest(/*bShowToast=*/false);
}
#endif
