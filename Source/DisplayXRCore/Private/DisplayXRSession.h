// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "DisplayXRTypes.h"

// Include our bundled OpenXR headers (not from UE's OpenXRHMD module)
#include <openxr/openxr.h>

#include "Native/displayxr_extensions.h"

/**
 * N-view atlas tile layout, populated from the DisplayXR runtime's rendering
 * mode info. Drives render target sizing, view rect placement, and view count.
 *
 * Mode 0 (2D): 1x1 tiles @ scale 1x1 -> single full-res view
 * Mode 1 (3D): 2x1 tiles @ scale 0.5x0.5 -> two views in atlas
 * Extensible to any NxM layout.
 */
struct FDisplayXRViewConfig
{
	int32 TileColumns = 1;
	int32 TileRows = 1;
	float ScaleX = 1.0f;
	float ScaleY = 1.0f;
	int32 DisplayPixelW = 1920;
	int32 DisplayPixelH = 1080;

	int32 GetViewCount() const { return TileColumns * TileRows; }
	int32 GetTileW() const { return FMath::RoundToInt(DisplayPixelW * ScaleX); }
	int32 GetTileH() const { return FMath::RoundToInt(DisplayPixelH * ScaleY); }
	int32 GetAtlasW() const { return TileColumns * GetTileW(); }
	int32 GetAtlasH() const { return TileRows * GetTileH(); }
};

/**
 * Cross-platform direct OpenXR session manager.
 *
 * Loads the DisplayXR OpenXR runtime directly (via LoadLibraryW on Windows,
 * dlopen on Mac), bypassing Unreal's OpenXR plugin entirely. Creates its own
 * XrInstance and XrSession, manages the OpenXR lifecycle, and provides
 * thread-safe access to eye tracking data and display info.
 *
 * Replaces both FDisplayXRExtensionPlugin (Windows/Android OpenXR hook path)
 * and FDisplayXRDirectSession (Mac direct path) with a single unified session.
 */
class FDisplayXRSession
{
public:
	FDisplayXRSession();
	~FDisplayXRSession();

	/** Initialize: load OpenXR loader, create instance + session. */
	bool Initialize();

	/** Shutdown: destroy session + instance, unload loader. */
	void Shutdown();

	/** Per-frame tick: poll events, locate views, store eye positions. */
	void Tick();

	/** Check if the session is active (instance created, display info available). */
	bool IsActive() const { return bActive; }

	/** Check if the OpenXR session handle has been created. */
	bool IsSessionCreated() const { return Session != XR_NULL_HANDLE; }

	// --- Data access (thread-safe via double buffering) ---

	/** Get display info (written once at init). */
	FDisplayXRDisplayInfo GetDisplayInfo() const { return DisplayInfo; }

	/** Get current view config (tile layout, driven by display mode). */
	FDisplayXRViewConfig GetViewConfig() const;

	/** Get latest eye positions in OpenXR display-local space (meters). */
	void GetEyePositions(FVector& OutLeft, FVector& OutRight, bool& bOutTracked) const;

	/** Request 2D or 3D display mode. Updates ViewConfig on success. */
	bool RequestDisplayMode(bool bMode3D);

	/** Create the OpenXR session with D3D graphics binding and HWND.
	 *  Must be called once the game viewport and RHI device are available. */
	bool CreateSessionWithGraphics(void* D3DDevice, void* CommandQueue, void* WindowHandle);

	// --- Accessors for compositor integration ---

	XrSession GetXrSession() const { return Session; }
	XrInstance GetXrInstance() const { return Instance; }
	XrSpace GetXrSpace() const { return ViewSpace; }
	PFN_xrGetInstanceProcAddr GetXrGetInstanceProcAddr() const { return xrGetInstanceProcAddrFunc; }
	bool IsSessionRunning() const { return bSessionRunning; }

	/** Store predicted display time from compositor thread for xrLocateViews. */
	void SetPredictedDisplayTime(int64 Time) { PredictedDisplayTime.Store(Time); }

	/** Set tunables from game thread. */
	void SetTunables(const FDisplayXRTunables& InTunables);

	/** Read tunables (for device to pass to Kooima). */
	FDisplayXRTunables GetTunables() const;

	/** Set scene transform from game thread. */
	void SetSceneTransform(const FTransform& InTransform, bool bEnabled);

	/** Get scene transform data for Kooima pose parameter. */
	void GetSceneTransform(FVector& OutPosition, FQuat& OutOrientation, bool& bOutEnabled) const;

private:
	bool LoadOpenXRLoader();
	void UnloadOpenXRLoader();
	bool CreateInstance();
	bool CreateSession();
	void QueryDisplayInfo();
	void QueryRenderingModes();
	void UpdateViewConfigFromDisplayMode(bool bMode3D);
	void LocateViews();

	// --- OpenXR state ---
	bool bActive = false;
	void* LoaderHandle = nullptr;

	XrInstance Instance = XR_NULL_HANDLE;
	XrSession Session = XR_NULL_HANDLE;
	XrSystemId SystemId = XR_NULL_SYSTEM_ID;
	XrSpace ViewSpace = XR_NULL_HANDLE;
	XrSessionState SessionState = XR_SESSION_STATE_UNKNOWN;
	bool bSessionRunning = false;

	// Function pointers (resolved via xrGetInstanceProcAddr)
	PFN_xrGetInstanceProcAddr xrGetInstanceProcAddrFunc = nullptr;
	PFN_xrRequestDisplayModeEXT xrRequestDisplayModeFunc = nullptr;
	PFN_xrEnumerateDisplayRenderingModesEXT xrEnumerateDisplayRenderingModesFunc = nullptr;

	// Display info (written once at init)
	FDisplayXRDisplayInfo DisplayInfo;

	// View config (updated on display mode change)
	FDisplayXRViewConfig ViewConfigBuffer[2];
	TAtomic<int32> ViewConfigReadIndex{0};

	// Double-buffered tunables
	FDisplayXRTunables TunablesBuffer[2];
	TAtomic<int32> TunablesReadIndex{0};

	// Scene transform
	struct FSceneTransformData
	{
		FVector Position = FVector::ZeroVector;
		FQuat Orientation = FQuat::Identity;
		FVector Scale = FVector::OneVector;
		bool bEnabled = false;
	};
	FSceneTransformData SceneTransformBuffer[2];
	TAtomic<int32> SceneTransformReadIndex{0};

	// Predicted display time from compositor thread's xrWaitFrame (for xrLocateViews)
	TAtomic<int64> PredictedDisplayTime{0};

	// Eye positions (written by Tick, read by game thread)
	struct FEyeData
	{
		FVector LeftEye = FVector::ZeroVector;
		FVector RightEye = FVector::ZeroVector;
		bool bTracked = false;
	};
	FEyeData EyeDataBuffer[2];
	TAtomic<int32> EyeDataReadIndex{0};
};
