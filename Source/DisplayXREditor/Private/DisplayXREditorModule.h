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
	bool TickPreview(float DeltaTime);

	TSharedPtr<FDisplayXRPreviewSession> PreviewSession;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
	FTSTicker::FDelegateHandle TickHandle;
};
