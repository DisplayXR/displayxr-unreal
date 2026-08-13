// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXREditorModule.h"
#include "DisplayXRPreviewSession.h"
#include "DisplayXRPIEPreview.h"
#include "DisplayXRPlatform.h"
#include "DisplayXRCoreModule.h"
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

// Debug escape hatch for the native path's OUTPUT stage: with native PIE on,
// the default is the weave-to-texture in-tab preview (#38); setting this to 1
// forces the legacy top-level mirror window instead. Also the automatic
// fallback when the runtime lacks XR_DXR_display_zones.
static TAutoConsoleVariable<int32> CVarEditorNativePIEMirror(
	TEXT("r.DisplayXR.EditorNativePIEMirror"),
	0,
	TEXT("0 = weave-to-texture preview in the PIE tab (default). 1 = force the legacy top-level mirror window."),
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

	// Defensive: editor may be closing during PIE. Same ordering as
	// OnPrePIEEnded — compositor down while its bound HWND is still valid.
	if (PreviewStartTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PreviewStartTicker);
		PreviewStartTicker.Reset();
	}
	if (PIEPreview.IsValid())
	{
		PIEPreview->Stop();
		PIEPreview.Reset();
	}
	StereoForcedViewport.Reset();
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;
	FDisplayXRCoreModule::NotifyPlaySessionEnded();
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

	// Output stage: weave-to-texture in-tab preview (#38) by default; the
	// legacy top-level mirror when forced by CVar or when the runtime lacks
	// display zones (the texture-mode canvas has nowhere to come from).
	FDisplayXRSession* Session = FDisplayXRCoreModule::GetSession();
	const bool bMirrorForced = CVarEditorNativePIEMirror.GetValueOnGameThread() != 0;
	const bool bHasZones = Session && Session->HasDisplayZones();

	if (!bMirrorForced && bHasZones)
	{
		// The PIE viewport has no cached geometry and no owning window until
		// Slate arranges it (next tick at the earliest), so the preview start
		// is deferred. Compositor creation must stay DISARMED until the proxy
		// exists — NotifyPlaySessionStarting fires inside the ticker — so an
		// early UpdateViewport cannot bind a compositor to the wrong window.
		//
		// A FRESH editor process starts ARMED (game mode depends on that
		// initial state), so the very first PIE draw — which happens before
		// our deferred TryStart tick — would otherwise bind a handle-mode
		// compositor to UE's own top-level window. Disarm explicitly first;
		// this also tears down any compositor a previous session leaked.
		FDisplayXRCoreModule::NotifyPlaySessionEnded();

		// Set BEFORE stereo enables: the flag drives
		// ShouldUseSeparateRenderTarget()==false, which must hold from the very
		// first stereo frame — a single separate-RT frame puts the editor
		// window on Slate's stereo-composite path (whole editor UI rendered
		// into the swapchain; see the slate-composite trap).
		FDisplayXRPlatform::bRequestSharedTextureBinding = true;

		PIEPreview = MakeShared<FDisplayXRPIEPreview>();
		PreviewStartAttempts = 0;
		TWeakPtr<SViewport> WeakViewport = ViewportWidget;
		PreviewStartTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[this, WeakViewport](float) -> bool
			{
				TSharedPtr<SViewport> VP = WeakViewport.Pin();
				if (!PIEPreview.IsValid() || !VP.IsValid())
				{
					PreviewStartTicker.Reset();
					return false; // PIE already ending — teardown owns cleanup
				}
				if (PIEPreview->TryStart(VP.ToSharedRef()))
				{
					FDisplayXRCoreModule::NotifyPlaySessionStarting();
					PreviewStartTicker.Reset();
					return false;
				}
				if (++PreviewStartAttempts > 600)
				{
					UE_LOG(LogDisplayXREditor, Warning,
						TEXT("DisplayXR: [NativePIE] viewport never got geometry/window — falling back to the mirror window"));
					PIEPreview.Reset();
					// Back to the separate-RT zero-copy path for the mirror.
					FDisplayXRPlatform::bRequestSharedTextureBinding = false;
					CreateMirrorWindow();
					FDisplayXRCoreModule::NotifyPlaySessionStarting();
					PreviewStartTicker.Reset();
					return false;
				}
				return true;
			}));
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] texture-mode in-tab preview arming (deferred start)"));
	}
	else
	{
		if (!bMirrorForced && !bHasZones)
		{
			UE_LOG(LogDisplayXREditor, Warning,
				TEXT("DisplayXR: [NativePIE] runtime does not advertise XR_DXR_display_zones — ")
				TEXT("in-tab weaved preview unavailable, using the legacy mirror window. ")
				TEXT("Update the DisplayXR runtime for the in-tab preview."));
		}

		// Create the raw-Win32 mirror BEFORE enabling stereo so the next
		// UpdateViewport call sees OverrideCompositorHWND set and binds the
		// compositor's session to the mirror HWND directly.
		CreateMirrorWindow();

		// Re-open deferred compositor creation. The previous PIE session left it
		// disarmed on purpose (see NotifyPlaySessionEnded), so without this the
		// second and later Play presses would never rebuild a compositor. Must come
		// after CreateMirrorWindow so the rebuild binds to the new mirror HWND.
		FDisplayXRCoreModule::NotifyPlaySessionStarting();
	}

	// Flip the flag UEngine::IsStereoscopic3D checks via FViewport::IsStereoRenderingAllowed().
	// PlayLevel.cpp:3377 sets this from bVRPreview at SPIEViewport construction; we do it
	// unconditionally after the fact because plain-PIE is our only intended Play mode.
	//
	// Remember WHICH widget: in "Selected Viewport" play mode PIE borrows the
	// level editor's own viewport widget, which survives PIE — the flag must be
	// flipped back at PIE end or the editor viewport keeps stereo-rendering
	// head-tracked SBS after Stop (our IsStereoEnabled() is unconditional).
	StereoForcedViewport = ViewportWidget;
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

	if (PreviewStartTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PreviewStartTicker);
		PreviewStartTicker.Reset();
	}

	if (PIEPreview.IsValid())
	{
		// Texture-mode teardown in dependency order (blit hook + render flush →
		// compositor while the proxy is still alive → platform flags → proxy
		// window) happens inside Stop().
		PIEPreview->Stop();
		PIEPreview.Reset();

		DisableForcedStereo();
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] PrePIEEnded — texture-mode preview torn down"));
		return;
	}

	// ---- legacy mirror path ----

	// Clear the override BEFORE disabling stereo so any trailing UpdateViewport
	// call doesn't re-latch the compositor to a dying HWND.
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;

	DisableForcedStereo();

	// Drop the compositor while the mirror HWND its session is bound to is still
	// alive — hence before DestroyMirrorWindow(). This also leaves compositor
	// creation disarmed until the next OnPostPIEStarted, so nothing rebuilds
	// against a window that is about to go away.
	FDisplayXRCoreModule::NotifyPlaySessionEnded();

	DestroyMirrorWindow();
	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] PrePIEEnded — stereo device disabled"));
}

void FDisplayXREditorModule::DisableForcedStereo()
{
	// The widget PIE borrowed survives PIE in "Selected Viewport" mode — the
	// stereo flag must come off or the LEVEL EDITOR viewport keeps stereo-
	// rendering head-tracked SBS after Stop (IsStereoEnabled is unconditional).
	if (TSharedPtr<SViewport> VP = StereoForcedViewport.Pin())
	{
		VP->EnableStereoRendering(false);
	}
	StereoForcedViewport.Reset();

	if (GEngine && GEngine->StereoRenderingDevice.IsValid())
	{
		GEngine->StereoRenderingDevice->EnableStereo(false);
	}

	// Hand the panel back to 2D. The XrSession survives PIE, so without an
	// explicit request the runtime's mode authority stays 3D and the lens
	// stays on over the 2D editor desktop after Stop.
	FDisplayXRPlatform::RequestDisplayMode(false);
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
