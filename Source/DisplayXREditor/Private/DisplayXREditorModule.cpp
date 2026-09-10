// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXREditorModule.h"
#include "DisplayXRPIEPreview.h"
#include "DisplayXRPlatform.h"
#include "DisplayXRCoreModule.h"
#include "DisplayXRCamera.h"
#include "DisplayXRDisplay.h"
#include "DisplayXRRigVisualizers.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXREditor, Log, All);

// The in-tab weaved PIE preview (#38): pressing Play drives UE's own stereo
// pipeline and shows the runtime's WOVEN output inside the PIE viewport tab.
// Default ON — this is how the editor works now; the CVar remains as an
// escape hatch (0 = no editor preview at all). Requires a runtime advertising
// XR_DXR_display_zones; without it the module warns once and does nothing.
static TAutoConsoleVariable<int32> CVarEditorNativePIE(
	TEXT("r.DisplayXR.EditorNativePIE"),
	1,
	TEXT("1 = weaved 3D preview inside the PIE viewport tab (default). 0 = disable the editor preview."),
	ECVF_Default);

static FORCEINLINE bool IsNativePIEEnabled()
{
	return CVarEditorNativePIE.GetValueOnGameThread() != 0;
}

void FDisplayXREditorModule::StartupModule()
{
	PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FDisplayXREditorModule::OnPostPIEStarted);
	PrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(this, &FDisplayXREditorModule::OnPrePIEEnded);

	// Rig gizmos: convergence plane (camera rig) and display plane (display rig),
	// drawn while the component is selected in the level or Blueprint viewport.
	if (GUnrealEd)
	{
		GUnrealEd->RegisterComponentVisualizer(UDisplayXRCamera::StaticClass()->GetFName(), MakeShared<FDisplayXRCameraVisualizer>());
		GUnrealEd->RegisterComponentVisualizer(UDisplayXRDisplay::StaticClass()->GetFName(), MakeShared<FDisplayXRDisplayVisualizer>());
	}

	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Editor module started (in-tab weaved preview on Play)"));
}

void FDisplayXREditorModule::ShutdownModule()
{
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

	FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
	FEditorDelegates::PrePIEEnded.Remove(PrePIEEndedHandle);

	if (GUnrealEd)
	{
		GUnrealEd->UnregisterComponentVisualizer(UDisplayXRCamera::StaticClass()->GetFName());
		GUnrealEd->UnregisterComponentVisualizer(UDisplayXRDisplay::StaticClass()->GetFName());
	}

	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Editor module shut down"));
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

	// The weave-to-texture canvas comes from a display zone — without the
	// extension there is nothing to preview into. Warn once per session and do
	// nothing: stereo stays off, the editor behaves like a plain 2D editor.
	FDisplayXRSession* Session = FDisplayXRCoreModule::GetSession();
	if (!Session || !Session->HasDisplayZones())
	{
		UE_LOG(LogDisplayXREditor, Warning,
			TEXT("DisplayXR: [NativePIE] runtime does not advertise XR_DXR_display_zones — ")
			TEXT("the in-tab weaved preview is unavailable. Update the DisplayXR runtime."));
		return;
	}

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

		// On a RESTART the borrowed level-viewport widget usually still has
		// valid geometry from the previous session — start synchronously and
		// skip the deferred tick entirely (shaves seconds off Play #2+; the
		// first Play of a session falls through to the ticker as before).
		if (PIEPreview->TryStart(ViewportWidget.ToSharedRef()))
		{
			FDisplayXRCoreModule::NotifyPlaySessionStarting();
			UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] texture-mode in-tab preview started synchronously"));
		}
		else
		{
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
				if (++PreviewStartAttempts % 60 == 0)
				{
					// Diagnose slow deferred starts: what is the widget missing?
					const FVector2D Size = VP->GetCachedGeometry().GetAbsoluteSize();
					const TSharedPtr<SWindow> Win = FSlateApplication::Get().FindWidgetWindow(VP.ToSharedRef());
					UE_LOG(LogDisplayXREditor, Log,
						TEXT("DisplayXR: [NativePIE] preview start still deferred (attempt %d): geometry=%.0fx%.0f window=%s"),
						PreviewStartAttempts, Size.X, Size.Y,
						Win.IsValid() ? *Win->GetTitle().ToString() : TEXT("<none>"));
				}
				if (PreviewStartAttempts > 600)
				{
					UE_LOG(LogDisplayXREditor, Warning,
						TEXT("DisplayXR: [NativePIE] viewport never got geometry/window — no preview this session"));
					// Undo everything OnPostPIEStarted forced on; UE returns to
					// plain mono rendering for the rest of the session.
					PIEPreview.Reset();
					FDisplayXRPlatform::bRequestSharedTextureBinding = false;
					if (TSharedPtr<SViewport> Forced = StereoForcedViewport.Pin())
					{
						Forced->EnableStereoRendering(false);
					}
					StereoForcedViewport.Reset();
					PreviewStartTicker.Reset();
					return false;
				}
				return true;
			}));
		UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: [NativePIE] texture-mode in-tab preview arming (deferred start)"));
		}
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

	// No preview this session (runtime without display zones, or the deferred
	// start never landed). Undo defensively; all of these no-op when nothing
	// was forced on.
	FDisplayXRPlatform::bRequestSharedTextureBinding = false;
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;
	DisableForcedStereo();
	FDisplayXRCoreModule::NotifyPlaySessionEnded();
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

IMPLEMENT_MODULE(FDisplayXREditorModule, DisplayXREditor)
