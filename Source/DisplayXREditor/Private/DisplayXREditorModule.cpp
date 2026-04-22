// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXREditorModule.h"
#include "DisplayXRPreviewSession.h"
#include "DisplayXRPlatform.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXREditor, Log, All);

// Phase 3 toggle: when 1, skip the SceneCapture preview and instead force the
// PIE SViewport's bEnableStereoRendering flag so FDisplayXRDevice drives the
// PIE render natively. Default 0 preserves the shipped SceneCapture path.
// Runtime-togglable: set via `r.DisplayXR.EditorNativePIE 1` in the editor
// console before pressing Play.
static TAutoConsoleVariable<int32> CVarEditorNativePIE(
	TEXT("r.DisplayXR.EditorNativePIE"),
	0,
	TEXT("0 = ship SceneCapture preview (default). 1 = experimental native XR PIE path."),
	ECVF_Default);

static FORCEINLINE bool IsNativePIEEnabled()
{
	return CVarEditorNativePIE.GetValueOnGameThread() != 0;
}

void FDisplayXREditorModule::StartupModule()
{
	BeginPIEHandle = FEditorDelegates::BeginPIE.AddRaw(this, &FDisplayXREditorModule::OnBeginPIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FDisplayXREditorModule::OnEndPIE);
	PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FDisplayXREditorModule::OnPostPIEStarted);
	PrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FDisplayXREditorModule::OnPrePIEEnded);

	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Editor module started (preview on Play)"));
}

void FDisplayXREditorModule::ShutdownModule()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (PreviewSession.IsValid())
	{
		PreviewSession->Stop();
		PreviewSession.Reset();
	}

	// Defensive: editor may be closing during PIE.
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;
	DestroyMirrorWindow();

	FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
	FEditorDelegates::PrePIEEnded.Remove(PrePIEEndedHandle);

	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Editor module shut down"));
}

void FDisplayXREditorModule::OnBeginPIE(bool bIsSimulating)
{
	if (IsNativePIEEnabled())
	{
		// Native PIE path: skip the SceneCapture preview. A second OpenXR session
		// from FDisplayXRPreviewSession would fight the one FDisplayXRDevice creates
		// in UpdateViewport once the PIE viewport allows stereo.
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] CVar on — skipping SceneCapture preview"));
		return;
	}

	PreviewSession = MakeShared<FDisplayXRPreviewSession>();
	if (PreviewSession->Start())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FDisplayXREditorModule::TickPreview));
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Preview session started on Play"));
	}
	else
	{
		UE_LOG(LogDisplayXREditor, Warning, TEXT("DisplayXR: Failed to start preview session on Play"));
		PreviewSession.Reset();
	}
}

void FDisplayXREditorModule::OnPostPIEStarted(bool bIsSimulating)
{
	if (!IsNativePIEEnabled())
	{
		return;
	}

	// FEditorDelegates::PostPIEStarted fires after SPIEViewport is constructed
	// (PlayLevel.cpp sequence: create viewport at ~line 3382, then broadcast
	// PostPIEStarted at ~line 2978 of the enclosing frame). Safe to look up.
	FWorldContext* PIEContext = GEditor ? GEditor->GetPIEWorldContext() : nullptr;
	UGameInstance* GameInstance = PIEContext ? PIEContext->OwningGameInstance : nullptr;
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;

	if (!ViewportWidget.IsValid())
	{
		UE_LOG(LogDisplayXREditor, Warning,
			TEXT("DisplayXR: [NativePIE] PostPIEStarted: could not reach SViewport (pieCtx=%p inst=%p vpc=%p)"),
			PIEContext, GameInstance, ViewportClient);
		return;
	}

	// Create the raw-Win32 mirror BEFORE enabling stereo so the next
	// UpdateViewport call sees OverrideCompositorHWND set and binds the
	// compositor's session to the mirror HWND directly.
	CreateMirrorWindow();

	// Flip the flag UEngine::IsStereoscopic3D checks via FViewport::IsStereoRenderingAllowed().
	// PlayLevel.cpp:3377 sets this from bVRPreview at SPIEViewport construction; we do it
	// unconditionally after the fact because plain-PIE is our only intended Play mode.
	ViewportWidget->EnableStereoRendering(true);

	// Mirror PlayLevel.cpp:3498: tell the stereo device to flip on. Our IsStereoEnabled()
	// already returns true unconditionally, so this is belt-and-suspenders, but matches
	// how Epic's OpenXRHMD path is driven for VR Preview.
	if (GEngine && GEngine->StereoRenderingDevice.IsValid())
	{
		GEngine->StereoRenderingDevice->EnableStereo(true);
	}

	UE_LOG(LogDisplayXREditor, Log,
		TEXT("DisplayXR: [NativePIE] Forced stereo on PIE viewport (widget=%p)"),
		ViewportWidget.Get());
}

void FDisplayXREditorModule::OnPrePIEEnded(bool bIsSimulating)
{
	if (!IsNativePIEEnabled())
	{
		return;
	}

	// Clear the override BEFORE disabling stereo so any trailing UpdateViewport
	// call doesn't re-latch the compositor to a dying HWND.
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;

	// The SPIEViewport is destroyed as part of PIE teardown, so we don't strictly
	// need to flip its flag off. Flip the device off for cleanliness; the next PIE
	// session will re-enable in OnPostPIEStarted.
	if (GEngine && GEngine->StereoRenderingDevice.IsValid())
	{
		GEngine->StereoRenderingDevice->EnableStereo(false);
	}

	DestroyMirrorWindow();
	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] PrePIEEnded — stereo device disabled"));
}

void FDisplayXREditorModule::OnEndPIE(bool bIsSimulating)
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (PreviewSession.IsValid())
	{
		PreviewSession->Stop();
		PreviewSession.Reset();
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Preview session stopped on End Play"));
	}
}

bool FDisplayXREditorModule::TickPreview(float DeltaTime)
{
	if (!PreviewSession.IsValid() || !PreviewSession->IsActive())
	{
		return true;
	}

	PreviewSession->Tick();
	return true;
}

// =============================================================================
// Mirror window (raw-Win32 top-level popup; runtime presents atlas into it)
// =============================================================================
#if PLATFORM_WINDOWS
static const wchar_t* MIRROR_CLASS = L"DisplayXRMirror";
static bool bMirrorClassReg = false;

static LRESULT CALLBACK MirrorWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_MOUSEACTIVATE:
		// Clicks on the mirror don't steal focus from the editor / PIE viewport.
		return MA_NOACTIVATE;
	case WM_CLOSE:
		// User close during PIE: no-op. PIE teardown owns DestroyWindow.
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

static bool RegisterMirrorClass()
{
	if (bMirrorClassReg) return true;
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = MirrorWndProc;
	wc.hInstance = GetModuleHandleW(0);
	wc.lpszClassName = MIRROR_CLASS;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
	bMirrorClassReg = true;
	return true;
}
#endif

void FDisplayXREditorModule::CreateMirrorWindow()
{
#if PLATFORM_WINDOWS
	if (MirrorHWND)
	{
		return;
	}

	const FDisplayXRDisplayInfo DI = FDisplayXRPlatform::GetDisplayInfo();
	MirrorWidth  = DI.DisplayPixelWidth  > 0 ? DI.DisplayPixelWidth  : 3840;
	MirrorHeight = DI.DisplayPixelHeight > 0 ? DI.DisplayPixelHeight : 2160;

	if (!RegisterMirrorClass())
	{
		UE_LOG(LogDisplayXREditor, Error, TEXT("DisplayXR: [NativePIE] Failed to register mirror window class"));
		return;
	}

	HWND PriorForeground = GetForegroundWindow();

	// Match the shipped FDisplayXRPreviewSession's window shape exactly —
	// proven to appear above the editor, show up in Alt+Tab, and accept the
	// runtime's native-compositor present.
	//   WS_OVERLAPPEDWINDOW: proper top-level window (caption + sysmenu +
	//     resizable frame + min/max). This is the style that shows up in
	//     Alt+Tab; WS_POPUP alone is excluded unless WS_EX_APPWINDOW is set,
	//     and also loses z-order fights against editor windows in practice.
	//   WS_EX_NOACTIVATE: clicking the mirror doesn't steal focus from PIE.
	//   WS_EX_TOPMOST: stays above the editor when both share the 3D monitor.
	MirrorHWND = CreateWindowExW(
		WS_EX_NOACTIVATE | WS_EX_TOPMOST,
		MIRROR_CLASS, L"DisplayXR Preview (native)",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, MirrorWidth, MirrorHeight,
		NULL, NULL, GetModuleHandleW(0), NULL);

	if (!MirrorHWND)
	{
		UE_LOG(LogDisplayXREditor, Error, TEXT("DisplayXR: [NativePIE] CreateWindowExW failed (err=%lu)"), GetLastError());
		return;
	}

	// Move the mirror onto the monitor whose native resolution matches the
	// DisplayXR display (first match wins; fallback is default position).
	struct EnumCtx { int32 W; int32 H; HMONITOR Found; } Ctx{ MirrorWidth, MirrorHeight, nullptr };
	EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMon, HDC, LPRECT, LPARAM Lp) -> BOOL {
		auto* Cx = reinterpret_cast<EnumCtx*>(Lp);
		MONITORINFO mi = { sizeof(mi) };
		if (GetMonitorInfo(hMon, &mi)) {
			const int32 W = mi.rcMonitor.right - mi.rcMonitor.left;
			const int32 H = mi.rcMonitor.bottom - mi.rcMonitor.top;
			if (W == Cx->W && H == Cx->H) { Cx->Found = hMon; return 0; }
		}
		return 1;
	}, reinterpret_cast<LPARAM>(&Ctx));
	if (Ctx.Found)
	{
		MONITORINFO mi = { sizeof(mi) };
		GetMonitorInfo(Ctx.Found, &mi);
		// HWND_TOPMOST here too so the z-order actually sticks above the editor
		// window (WS_EX_TOPMOST at creation is sometimes not enough if the
		// editor HWND was already topmost or more recently activated).
		SetWindowPos((HWND)MirrorHWND, HWND_TOPMOST,
			mi.rcMonitor.left, mi.rcMonitor.top, MirrorWidth, MirrorHeight,
			SWP_NOACTIVATE);
		UE_LOG(LogDisplayXREditor, Log,
			TEXT("DisplayXR: [NativePIE] Mirror placed on 3D monitor at (%ld,%ld) %dx%d"),
			mi.rcMonitor.left, mi.rcMonitor.top, MirrorWidth, MirrorHeight);
	}

	ShowWindow((HWND)MirrorHWND, SW_SHOWNOACTIVATE);

	// Return focus to the editor so PIE keeps receiving keyboard input.
	if (PriorForeground && PriorForeground != (HWND)MirrorHWND)
	{
		SetForegroundWindow(PriorForeground);
	}

	// Hand the HWND to the compositor via the platform hook. Next
	// UpdateViewport picks it up and binds the OpenXR session there.
	FDisplayXRPlatform::OverrideCompositorHWND = MirrorHWND;

	UE_LOG(LogDisplayXREditor, Log,
		TEXT("DisplayXR: [NativePIE] Mirror window %dx%d HWND=%p"),
		MirrorWidth, MirrorHeight, MirrorHWND);
#endif
}

void FDisplayXREditorModule::DestroyMirrorWindow()
{
#if PLATFORM_WINDOWS
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;
	if (MirrorHWND)
	{
		DestroyWindow((HWND)MirrorHWND);
		MirrorHWND = nullptr;
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] Mirror window destroyed"));
	}
#endif
}

IMPLEMENT_MODULE(FDisplayXREditorModule, DisplayXREditor)
