// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
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

	/** Editor weave-to-texture PIE preview (#38): when true (set by the editor
	 *  module together with OverrideCompositorHWND), the compositor binds the
	 *  session in TEXTURE mode — the runtime weaves into a plugin-created shared
	 *  D3D12 texture and the override HWND is only the display processor's
	 *  interlace-phase anchor (an invisible proxy that never presents). Read
	 *  once at compositor Initialize. */
	DISPLAYXRCORE_API static bool bRequestSharedTextureBinding;

	/** Zone size for the texture-mode preview, packed (W << 32 | H), in
	 *  proxy-window client pixels. One full-window zone: the texture-mode weave
	 *  is canvas-driven and the canvas derives from the submitted zones — with
	 *  no zone the display processor treats the canvas as "fill the whole
	 *  target" (the entire worst-case texture), magnifying the weave. Written
	 *  on the game thread by the editor glue; read by the session (locate
	 *  chain) and the compositor thread (xrEndFrame chain). 0 = no zone. */
	DISPLAYXRCORE_API static TAtomic<uint64> EditorZoneSizePacked;

	static void SetEditorZoneSize(uint32 W, uint32 H)
	{
		EditorZoneSizePacked.Store(((uint64)W << 32) | (uint64)H);
	}

	static bool GetEditorZoneSize(uint32& OutW, uint32& OutH)
	{
		const uint64 Packed = EditorZoneSizePacked.Load();
		OutW = (uint32)(Packed >> 32);
		OutH = (uint32)(Packed & 0xffffffffu);
		return OutW > 0 && OutH > 0;
	}

	/** The OS foreground window captured at module load (PostConfigInit) — under
	 *  the shell this is the shell/launcher that spawned us. UE grabs foreground
	 *  when it shows its game window on launch, which makes the shell stop
	 *  displaying the app until the user alt-tabs back; the compositor hands
	 *  foreground back to this window once it hides UE's window. Win32 HWND as
	 *  void*; null off-Windows / outside a shell session. */
	DISPLAYXRCORE_API static void* SavedShellForegroundHWND;
};
