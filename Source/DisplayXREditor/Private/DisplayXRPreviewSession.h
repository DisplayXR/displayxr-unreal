// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "DisplayXRTypes.h"

// Bundled OpenXR headers (reachable via PrivateIncludePaths in Build.cs)
#include <openxr/openxr.h>
#include "displayxr_extensions.h"

/**
 * View config for editor preview (local copy of FDisplayXRViewConfig from DisplayXRSession.h,
 * which lives in DisplayXRCore Private and is not accessible from DisplayXREditor).
 */
struct FDisplayXRPreviewViewConfig
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
 * Standalone OpenXR session for editor preview.
 *
 * Creates its own native window (like Unity's standalone preview), loads the
 * DisplayXR runtime, runs SceneCapture rendering from rig components, and
 * submits frames. Started/stopped by the editor module on PIE begin/end.
 *
 * The native window handles WM_MOVE/WM_SIZE to keep the runtime's weaver
 * aligned, TAB to cycle rigs, and WM_CLOSE to stop PIE.
 */
class FDisplayXRPreviewSession
{
public:
	FDisplayXRPreviewSession();
	~FDisplayXRPreviewSession();

	bool Start();
	void Stop();
	void Tick();
	bool IsActive() const { return bActive; }

	void CycleRig();
	FString GetActiveRigName() const;

	/** Called from native window's WndProc. */
	void OnNativeWindowClosed();
	void UpdateCanvasRect();

	/** Transfer Slate keyboard focus to the PIE viewport. Called when the
	 *  user clicks the preview window so keystrokes reach the possessed
	 *  pawn while the preview remains visible. */
	void FocusPIEViewport();

	/** Forward a raw mouse delta (in pixels, screen-relative) to the PIE
	 *  player controller's rotation input. Called while the user drags
	 *  with LMB held on the preview window. */
	void ForwardMouseLookDelta(int DeltaX, int DeltaY);

private:
	// --- Lifecycle ---
	bool LoadOpenXRRuntime();
	void UnloadOpenXRRuntime();
	bool CreateXrInstance();
	void DestroyXrInstance();
	bool CreateXrSession();
	void DestroyXrSession();
	bool CreateSwapchain();
	void DestroySwapchain();
	bool CreateRuntimeQueue();
	bool ResolveXrFunctions();
	void QueryDisplayInfo();
	void QueryRenderingModes();
	bool CreateNativeWindow();
	void DestroyNativeWindow();

	bool bActive = false;

	// --- Runtime loading ---
	void* LoaderHandle = nullptr;
	PFN_xrGetInstanceProcAddr xrGetInstanceProcAddrFunc = nullptr;

	// --- OpenXR state ---
	XrInstance Instance = XR_NULL_HANDLE;
	XrSession Session = XR_NULL_HANDLE;
	XrSystemId SystemId = XR_NULL_SYSTEM_ID;
	XrSpace ViewSpace = XR_NULL_HANDLE;
	bool bSessionRunning = false;

	// --- Display info ---
	FDisplayXRDisplayInfo DisplayInfo;
	FDisplayXRPreviewViewConfig ViewConfig;

	// --- D3D12 resources (Windows only) ---
	void* D3DDevice = nullptr;
	void* RuntimeQueue = nullptr;

	// --- Native preview window ---
	void* PreviewHWND = nullptr;  // standalone WS_OVERLAPPEDWINDOW
	bool bWindowClosed = false;

	// --- Canvas rect (for xrSetSharedTextureOutputRectEXT) ---
	PFN_xrSetSharedTextureOutputRectEXT xrSetOutputRectFunc = nullptr;

	// --- Swapchain ---
	XrSwapchain Swapchain = XR_NULL_HANDLE;
	struct FSwapchainImage { void* D3D12Resource = nullptr; };
	TArray<FSwapchainImage> SwapchainImages;
	uint32 SwapchainWidth = 0;
	uint32 SwapchainHeight = 0;
	int64 SwapchainFormat = 0;

	// --- Swapchain RHI wrappers ---
	TArray<FTextureRHIRef> SwapchainImagesRHI;
	bool bSwapchainWrapped = false;

	// --- Scene capture ---
	class UTextureRenderTarget2D* LeftRenderTarget = nullptr;
	class UTextureRenderTarget2D* RightRenderTarget = nullptr;
	class AActor* CaptureActor = nullptr;
	class USceneCaptureComponent2D* LeftCapture = nullptr;
	class USceneCaptureComponent2D* RightCapture = nullptr;

	bool CreateSceneCaptures();
	void DestroySceneCaptures();
	bool WrapSwapchainImagesAsRHI();
	void RenderAndBlit(uint32_t ImageIndex);
	void ScanForRigs();

	// --- Rig selection ---
	struct FRigEntry
	{
		TWeakObjectPtr<class UCameraComponent> Camera;
		TWeakObjectPtr<class UDisplayXRCamera> CameraRig;
		TWeakObjectPtr<class UDisplayXRDisplay> DisplayRig;
	};
	TArray<FRigEntry> DiscoveredRigs;
	int32 ActiveRigIndex = -1;

	// --- Frame state ---
	XrTime PredictedDisplayTime = 0;
	int32 FrameCount = 0;

	// --- DisplayXR extension function pointers ---
	PFN_xrRequestDisplayModeEXT xrRequestDisplayModeFunc = nullptr;
	PFN_xrEnumerateDisplayRenderingModesEXT xrEnumerateDisplayRenderingModesFunc = nullptr;

	// --- OpenXR frame loop function pointers ---
	typedef XrResult(XRAPI_PTR* PFN_xrWaitFrame)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
	typedef XrResult(XRAPI_PTR* PFN_xrBeginFrame)(XrSession, const XrFrameBeginInfo*);
	typedef XrResult(XRAPI_PTR* PFN_xrEndFrame)(XrSession, const XrFrameEndInfo*);
	typedef XrResult(XRAPI_PTR* PFN_xrEnumerateSwapchainFormats)(XrSession, uint32_t, uint32_t*, int64_t*);
	typedef XrResult(XRAPI_PTR* PFN_xrCreateSwapchain)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
	typedef XrResult(XRAPI_PTR* PFN_xrDestroySwapchain)(XrSwapchain);
	typedef XrResult(XRAPI_PTR* PFN_xrEnumerateSwapchainImages)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
	typedef XrResult(XRAPI_PTR* PFN_xrAcquireSwapchainImage)(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
	typedef XrResult(XRAPI_PTR* PFN_xrWaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo*);
	typedef XrResult(XRAPI_PTR* PFN_xrReleaseSwapchainImage)(XrSwapchain, const XrSwapchainImageReleaseInfo*);
	typedef XrResult(XRAPI_PTR* PFN_xrLocateViews)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);

	PFN_xrWaitFrame xrWaitFrameFunc = nullptr;
	PFN_xrBeginFrame xrBeginFrameFunc = nullptr;
	PFN_xrEndFrame xrEndFrameFunc = nullptr;
	PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormatsFunc = nullptr;
	PFN_xrCreateSwapchain xrCreateSwapchainFunc = nullptr;
	PFN_xrDestroySwapchain xrDestroySwapchainFunc = nullptr;
	PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImagesFunc = nullptr;
	PFN_xrAcquireSwapchainImage xrAcquireSwapchainImageFunc = nullptr;
	PFN_xrWaitSwapchainImage xrWaitSwapchainImageFunc = nullptr;
	PFN_xrReleaseSwapchainImage xrReleaseSwapchainImageFunc = nullptr;
	PFN_xrLocateViews xrLocateViewsFunc = nullptr;
};
