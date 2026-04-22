// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

class FDisplayXRPreviewSession;

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
	// XR_EXT_win32_window_binding + xrSetSharedTextureOutputRectEXT. No
	// app-side swapchain, no Slate — opaque presentation is handled by the
	// DisplayXR runtime (see displayxr-runtime-pvt issue #163).
	void CreateMirrorWindow();
	void DestroyMirrorWindow();

	TSharedPtr<FDisplayXRPreviewSession> PreviewSession;
	void* MirrorHWND = nullptr;
	int32 MirrorWidth = 0;
	int32 MirrorHeight = 0;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle PostPIEStartedHandle;
	FDelegateHandle PrePIEEndedHandle;
	FTSTicker::FDelegateHandle TickHandle;
};
