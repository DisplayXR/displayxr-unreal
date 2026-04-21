// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "IHeadMountedDisplayModule.h"

class FDisplayXRSession;

/**
 * DisplayXR core module. Registers as an HMD module so UE picks our custom
 * FDisplayXRDevice as the active HMD (with higher priority than OpenXR/SteamVR).
 *
 * Creates and owns the FDisplayXRSession (direct OpenXR runtime connection).
 */
class FDisplayXRCoreModule : public IHeadMountedDisplayModule
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// IHeadMountedDisplayModule
	virtual FString GetModuleKeyName() const override { return TEXT("DisplayXRCore"); }
	virtual bool IsHMDConnected() override;
	virtual TSharedPtr<IXRTrackingSystem, ESPMode::ThreadSafe> CreateTrackingSystem() override;
	virtual void GetModuleAliases(TArray<FString>& AliasesOut) const override;

	/** Get the session (for FDisplayXRPlatform routing). */
	static FDisplayXRSession* GetSession();

private:
	TSharedPtr<FDisplayXRSession> Session;
	static FDisplayXRCoreModule* ModuleInstance;
};
