// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "IHeadMountedDisplayModule.h"
#include "RHIResources.h"

class FAutoConsoleCommand;
class FDisplayXRSession;
class FDisplayXRDevInputProcessor;

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
	DISPLAYXRCORE_API static FDisplayXRSession* GetSession();

	/**
	 * Tell the active DisplayXR stereo device that a play session is starting,
	 * re-opening deferred compositor creation.
	 *
	 * Pairs with NotifyPlaySessionEnded(). Without it the second PIE run in an
	 * editor session never gets a compositor and stereo silently stays off.
	 * No-op when DisplayXR is not the active XR system. Game thread only.
	 */
	DISPLAYXRCORE_API static void NotifyPlaySessionStarting();

	/**
	 * Tell the active DisplayXR stereo device that a play session ended, so it
	 * drops its compositor while the window it is bound to is still alive.
	 *
	 * Called by the editor module when PIE ends, before the preview window is
	 * destroyed. No-op when DisplayXR is not the active XR system. Game thread
	 * only.
	 */
	DISPLAYXRCORE_API static void NotifyPlaySessionEnded();

	/**
	 * Texture-mode preview (#38): the woven texture the runtime weaves into,
	 * wrapped for UE sampling. Invalid until the compositor has created the
	 * shared surface, and again after session-end teardown. Game thread only —
	 * the editor presenter polls this and hands the ref to the render thread
	 * via a render command.
	 */
	DISPLAYXRCORE_API static FTextureRHIRef GetWovenTextureRHI_GameThread();

private:
	void RegisterDevInputProcessor();

	TSharedPtr<FDisplayXRSession> Session;
	TSharedPtr<FDisplayXRDevInputProcessor> DevInputProcessor;
	TUniquePtr<FAutoConsoleCommand> AtlasCaptureCmd;
	FDelegateHandle PostEngineInitHandle;
	static FDisplayXRCoreModule* ModuleInstance;
};
