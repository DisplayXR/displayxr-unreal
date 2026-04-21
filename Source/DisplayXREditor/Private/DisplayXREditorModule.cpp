// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXREditorModule.h"
#include "DisplayXRPreviewSession.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXREditor, Log, All);

void FDisplayXREditorModule::StartupModule()
{
	BeginPIEHandle = FEditorDelegates::BeginPIE.AddRaw(this, &FDisplayXREditorModule::OnBeginPIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FDisplayXREditorModule::OnEndPIE);

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

	UE_LOG(LogDisplayXREditor, Log, TEXT("DisplayXR: Editor module shut down"));
}

void FDisplayXREditorModule::OnBeginPIE(bool bIsSimulating)
{
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
