// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

class FDisplayXRPIEPreview;

/**
 * Editor-side glue for the in-tab weaved PIE preview (#38): pressing Play
 * forces stereo on the PIE viewport so FDisplayXRDevice drives the render, and
 * FDisplayXRPIEPreview shows the runtime's woven output inside the tab.
 */
class FDisplayXREditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnPostPIEStarted(bool bIsSimulating);
	void OnPrePIEEnded(bool bIsSimulating);

	// Undo everything OnPostPIEStarted forced on: the borrowed viewport
	// widget's stereo flag, the stereo device, and the panel's display mode
	// (the XrSession survives PIE, so without an explicit 2D request the
	// runtime's mode authority stays 3D and the lens stays on after Stop).
	void DisableForcedStereo();

	FDelegateHandle PostPIEStartedHandle;
	FDelegateHandle PrePIEEndedHandle;

	// Weave-to-texture PIE preview (#38): woven output inside the PIE viewport
	// tab. Created in OnPostPIEStarted when the runtime has display zones; its
	// start is deferred by a ticker until Slate has arranged the viewport
	// (geometry + owning window).
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
