// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRCoreModule.h"
#include "DisplayXRSession.h"
#include "DisplayXRPlatform.h"
#include "Rendering/DisplayXRDevice.h"
#include "SceneViewExtension.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellscalingapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#pragma comment(lib, "Shcore.lib")
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRCore, Log, All);

FDisplayXRCoreModule* FDisplayXRCoreModule::ModuleInstance = nullptr;

void FDisplayXRCoreModule::StartupModule()
{
	ModuleInstance = this;

#if PLATFORM_WINDOWS
	// Request per-monitor DPI awareness so the backbuffer uses physical pixels
	// (e.g., 3840x2160) instead of DPI-scaled logical pixels (e.g., 1536x864).
	// Must happen before rendering startup (PostConfigInit is early enough).
	SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Set DPI awareness to per-monitor"));
#endif

	// Set HMD priority higher than OpenXR/SteamVR so UE picks us
	float HighestXrPluginPriority = 0.0f;
	TArray<FString> XrPlugins = { TEXT("OpenXRHMD"), TEXT("SteamVR") };
	for (const FString& XrPlugin : XrPlugins)
	{
		float PluginPriority = 0.0f;
		GConfig->GetFloat(TEXT("HMDPluginPriority"), *XrPlugin, PluginPriority, GEngineIni);
		HighestXrPluginPriority = FMath::Max(PluginPriority, HighestXrPluginPriority);
	}
	GConfig->SetFloat(TEXT("HMDPluginPriority"), TEXT("DisplayXRCore"),
		HighestXrPluginPriority + 10, GEngineIni);

	// Create session (loads DisplayXR OpenXR runtime directly)
	Session = MakeShared<FDisplayXRSession>();
	if (!Session->Initialize())
	{
		UE_LOG(LogDisplayXRCore, Warning, TEXT("DisplayXR: Failed to initialize session"));
	}
	else
	{
		UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Session initialized"));
	}

	// Register with UE's HMD module discovery
	IHeadMountedDisplayModule::StartupModule();

	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Core module started (custom HMD path)"));
}

void FDisplayXRCoreModule::ShutdownModule()
{
	if (Session.IsValid())
	{
		Session->Shutdown();
		Session.Reset();
	}

	ModuleInstance = nullptr;
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Core module shut down"));

	IHeadMountedDisplayModule::ShutdownModule();
}

bool FDisplayXRCoreModule::IsHMDConnected()
{
	return Session.IsValid() && Session->IsActive();
}

TSharedPtr<IXRTrackingSystem, ESPMode::ThreadSafe> FDisplayXRCoreModule::CreateTrackingSystem()
{
	if (!Session.IsValid() || !Session->IsActive())
	{
		UE_LOG(LogDisplayXRCore, Warning, TEXT("DisplayXR: Cannot create tracking system, session not active"));
		return nullptr;
	}

	auto Device = FSceneViewExtensions::NewExtension<FDisplayXRDevice>(Session.Get());
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Tracking system created"));
	return Device;
}

void FDisplayXRCoreModule::GetModuleAliases(TArray<FString>& AliasesOut) const
{
	AliasesOut.Add(TEXT("DisplayXR"));
}

FDisplayXRSession* FDisplayXRCoreModule::GetSession()
{
	if (ModuleInstance && ModuleInstance->Session.IsValid())
	{
		return ModuleInstance->Session.Get();
	}
	return nullptr;
}

bool FDisplayXRPlatform::bSuppressCompositor = false;
void* FDisplayXRPlatform::OverrideCompositorHWND = nullptr;

IMPLEMENT_MODULE(FDisplayXRCoreModule, DisplayXRCore)
