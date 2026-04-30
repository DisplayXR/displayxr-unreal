// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"

/**
 * One-shot CPU readback of the current OpenXR swapchain atlas to a PNG.
 *
 * Port of the runtime-pvt reference (test_apps/common/atlas_capture_*.cpp): on
 * trigger, copies the full NxM-tile atlas off the swapchain texture and writes
 * %USERPROFILE%\Pictures\DisplayXR\<ProjectName>-<N>_<cols>x<rows>.png, with a
 * brief Win32 flash overlay for affordance.
 *
 * Trigger from any thread via RequestCapture() (game thread, console command,
 * Slate input preprocessor, Blueprint). The render-thread processor consumes
 * pending requests inside FDisplayXRDevice::PostRenderViewFamily_RenderThread.
 */
class FDisplayXRAtlasCapture
{
public:
	/** Arm one capture for the next-rendered atlas. Safe from any thread. */
	static void RequestCapture();

	/**
	 * Render-thread: if an arm is pending, read back AtlasSrc[0..AtlasW, 0..AtlasH]
	 * and emit a PNG. AtlasW/H/Cols/Rows come from FDisplayXRViewConfig (the atlas
	 * is a sub-rect of the full-display swapchain image).
	 */
	static void ProcessRequest_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		FRHITexture* AtlasSrc,
		int32 AtlasW,
		int32 AtlasH,
		int32 Cols,
		int32 Rows);
};
