// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

class FDisplayXRPreviewSession;
class FDisplayXRPIEPreview;

class FDisplayXREditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnBeginPIE(bool bIsSimulating);
	void OnEndPIE(bool bIsSimulating);
	void OnPostPIEStarted(bool bIsSimulating);
	void OnPrePIEEnded(bool bIsSimulating);
	bool TickPreview(float DeltaTime);

	// Native-PIE mirror: raw-Win32 top-level popup on the 3D display. The
	// runtime's native compositor presents the atlas into this HWND via
	// XR_DXR_win32_window_binding. No app-side
	// swapchain, no Slate — opaque presentation is handled by the
	// DisplayXR runtime (see displayxr-runtime-pvt issue #163).
	void CreateMirrorWindow();
	void DestroyMirrorWindow();

	// Undo everything OnPostPIEStarted forced on: the borrowed viewport
	// widget's stereo flag, the stereo device, and the panel's display mode
	// (the XrSession survives PIE, so without an explicit 2D request the
	// runtime's mode authority stays 3D and the lens stays on after Stop).
	void DisableForcedStereo();

	TSharedPtr<FDisplayXRPreviewSession> PreviewSession;
	void* MirrorHWND = nullptr;
	int32 MirrorWidth = 0;
	int32 MirrorHeight = 0;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle PostPIEStartedHandle;
	FDelegateHandle PrePIEEndedHandle;
	FTSTicker::FDelegateHandle TickHandle;

	// Weave-to-texture PIE preview (#38): woven output inside the PIE viewport
	// tab. Created in OnPostPIEStarted when the runtime has display zones and
	// the mirror escape hatch is off; its start is deferred by a ticker until
	// Slate has arranged the viewport (geometry + owning window).
	TSharedPtr<FDisplayXRPIEPreview> PIEPreview;
	FTSTicker::FDelegateHandle PreviewStartTicker;
	int32 PreviewStartAttempts = 0;

	// The viewport widget we forced bEnableStereoRendering=true on at Play. In
	// "Selected Viewport" play mode this is the LEVEL EDITOR's own viewport
	// widget (PIE borrows it — no separate SPIEViewport exists), so the flag
	// MUST be flipped back at PIE end or the editor viewport keeps stereo-
	// rendering head-tracked SBS after Stop.
	TWeakPtr<class SViewport> StereoForcedViewport;
};
