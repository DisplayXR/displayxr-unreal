// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Platform abstraction: routes calls to FDisplayXRSession via
// FDisplayXRCoreModule::GetSession().

#pragma once

#include "CoreMinimal.h"
#include "DisplayXRTypes.h"
#include "DisplayXRCoreModule.h"
#include "DisplayXRSession.h"

/**
 * Static helper that routes API calls to the active session.
 * Components and Blueprint functions call these instead of the session directly.
 */
struct FDisplayXRPlatform
{
	static void SetTunables(const FDisplayXRTunables& T)
	{
		if (auto* S = FDisplayXRCoreModule::GetSession()) S->SetTunables(T);
	}

	static void SetSceneTransform(const FTransform& Transform, bool bEnabled)
	{
		if (auto* S = FDisplayXRCoreModule::GetSession()) S->SetSceneTransform(Transform, bEnabled);
	}

	static FDisplayXRDisplayInfo GetDisplayInfo()
	{
		if (auto* S = FDisplayXRCoreModule::GetSession()) return S->GetDisplayInfo();
		return FDisplayXRDisplayInfo();
	}

	static void GetEyePositions(FVector& OutLeft, FVector& OutRight, bool& bOutTracked)
	{
		if (auto* S = FDisplayXRCoreModule::GetSession()) { S->GetEyePositions(OutLeft, OutRight, bOutTracked); return; }
		OutLeft = OutRight = FVector::ZeroVector;
		bOutTracked = false;
	}

	static bool RequestDisplayMode(bool bMode3D)
	{
		if (auto* S = FDisplayXRCoreModule::GetSession()) return S->RequestDisplayMode(bMode3D);
		return false;
	}

	static bool RequestEyeTrackingMode(bool bManual)
	{
		// TODO: implement when eye tracking mode extension is defined
		return false;
	}

	static bool IsAvailable()
	{
		auto* S = FDisplayXRCoreModule::GetSession();
		return S && S->IsActive();
	}

	/** Set by editor module to suppress game-mode compositor creation during PIE
	 *  when the standalone preview session handles 3D output instead. */
	DISPLAYXRCORE_API static bool bSuppressCompositor;

	/** Override HWND for compositor: if non-null, the compositor uses this window
	 *  instead of the game viewport's HWND. Set by the editor preview module. */
	DISPLAYXRCORE_API static void* OverrideCompositorHWND;

	/** The OS foreground window captured at module load (PostConfigInit) — under
	 *  the shell this is the shell/launcher that spawned us. UE grabs foreground
	 *  when it shows its game window on launch, which makes the shell stop
	 *  displaying the app until the user alt-tabs back; the compositor hands
	 *  foreground back to this window once it hides UE's window. Win32 HWND as
	 *  void*; null off-Windows / outside a shell session. */
	DISPLAYXRCORE_API static void* SavedShellForegroundHWND;
};
