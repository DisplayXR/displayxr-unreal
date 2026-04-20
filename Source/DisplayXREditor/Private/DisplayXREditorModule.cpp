// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXREditorModule.h"
#include "DisplayXRPreviewSession.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"

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

	// The SPIEViewport is destroyed as part of PIE teardown, so we don't strictly
	// need to flip its flag off. Flip the device off for cleanliness; the next PIE
	// session will re-enable in OnPostPIEStarted.
	if (GEngine && GEngine->StereoRenderingDevice.IsValid())
	{
		GEngine->StereoRenderingDevice->EnableStereo(false);
	}
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

IMPLEMENT_MODULE(FDisplayXREditorModule, DisplayXREditor)
