// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"

/**
 * One-shot capture of the runtime's multi-view atlas to a PNG.
 *
 * The GPU->CPU readback now lives in the runtime: RequestCapture() calls
 * xrCaptureAtlasEXT (XR_EXT_atlas_capture) via the active FDisplayXRSession, and
 * the runtime writes %USERPROFILE%\Pictures\DisplayXR\<Project>-<N>_<cols>x<rows>_atlas.png
 * from its own compositor atlas. The plugin keeps only the app-side UX:
 * filename numbering and a brief Win32 flash overlay.
 *
 * Trigger from any thread via RequestCapture() (game thread, console command,
 * Slate input preprocessor, Blueprint).
 */
class FDisplayXRAtlasCapture
{
public:
	/** Capture the current atlas to a PNG via the runtime. Safe from any thread. */
	static void RequestCapture();
};
