// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"
#include "DisplayXRSession.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"

/**
 * DisplayXR compositor integration with dedicated frame submission thread.
 *
 * Zero-copy pipeline: UE renders directly into OpenXR swapchain images.
 *
 *   Compositor thread (loop):
 *     1. xrWaitFrame  (blocks for vsync — off UE threads)
 *     2. xrBeginFrame
 *     3. Trigger BeginFrameReadyEvent
 *     4. Wait EndFrameReadyEvent (up to 50ms)
 *     5. xrEndFrame with projection layer (or empty frame on timeout)
 *
 *   Game thread (AcquireColorTexture):
 *     - Wait BeginFrameReadyEvent
 *     - xrAcquireSwapchainImage + xrWaitSwapchainImage
 *     - Return image index to UE
 *
 *   Render thread (after UE scene render):
 *     - Transition swapchain texture to Present
 *     - xrReleaseSwapchainImage
 *     - Trigger EndFrameReadyEvent
 */
class FCompositorRunnable;

class FDisplayXRCompositor
{
	friend class FCompositorRunnable;
public:
	FDisplayXRCompositor(FDisplayXRSession* InSession);
	~FDisplayXRCompositor();

	bool Initialize(void* ParentHWND, void* D3DDevice, void* CommandQueue);
	void Shutdown();

	/** Check if compositor is initialized and thread is running. */
	bool IsReady() const { return bReady; }

	/** Tick: handle deferred swapchain creation after session is running. */
	void Tick();

	// --- UE render target integration ---

	/** Game thread: fill OutImages with wrapped swapchain FRHITextures. */
	bool GetSwapchainImagesRHI(TArray<FTextureRHIRef>& OutImages) const;

	/** Game thread: acquire next swapchain image. Returns image index or -1 on failure. */
	int32 AcquireImage_GameThread();

	/** Render thread: release the acquired swapchain image and signal compositor to call xrEndFrame. */
	void ReleaseImage_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* SwapchainTexture);

	uint32 GetSwapchainWidth() const { return SwapchainWidth; }
	uint32 GetSwapchainHeight() const { return SwapchainHeight; }

	/** The HWND bound to the OpenXR session (the child overlay, or the parent in
	 *  the editor-override case). Under the shell the RUNTIME sizes this to the
	 *  workspace window, so UE + the plugin must measure THIS window (not UE's
	 *  own top-level window) for render/tile dims, else content can't track a
	 *  workspace resize. */
	void* GetBoundHWND() const { return ChildHWND ? ChildHWND : ParentHWND; }

	/** True on the IPC array path: each eye renders at the FIXED per-view slice
	 *  size (content fills the slice). AdjustViewRect must NOT window-scale tiles
	 *  there, else a smaller-than-display window underfills the fixed slice
	 *  (black band / shifted tile). Window-relative perspective is via Kooima. */
	bool UsesArrayCopyPath() const { return bUseCopyPath; }

private:
	bool CreateChildWindow(void* InParentHWND);
	void DestroyChildWindow();
	// Window-relative per-view tile dims from ParentHWND's client rect (matches the
	// device's AdjustViewRect math). The IPC array copy AND projection both call
	// this so UE's rendered tile, the copy source rect, and the submitted imageRect
	// all agree → content aspect tracks the window (correct under resize).
	void ComputeTileDims(int32& OutTW, int32& OutTH) const;
	bool CreateSwapchain();
	void DestroySwapchain();
	bool WrapSwapchainImagesAsRHI();
	bool CreateRuntimeQueue();
	bool ResolveXrFunctions();
	bool StartCompositorThread();
	void StopCompositorThread();

	// The compositor thread's main loop
	void CompositorLoop();

	FDisplayXRSession* Session = nullptr;
	bool bReady = false;
	bool bSwapchainCreated = false;

	// Child window
	void* ChildHWND = nullptr;
	void* ParentHWND = nullptr;
	// Shell: set once after clipping ParentHWND to an empty region to hide UE's
	// top-level mono window from the desktop without moving/resizing/restyling it.
	bool bWorkspaceWindowHidden = false;

	// UE's D3D12 device (borrowed, not owned) + dedicated runtime queue (owned)
	void* UEDevice = nullptr;
	void* UECommandQueue = nullptr;    // UE's main queue (not given to runtime)
	void* RuntimeQueue = nullptr;      // ID3D12CommandQueue* on UEDevice, for xrCreateSession

	// OpenXR swapchain
	XrSwapchain Swapchain = XR_NULL_HANDLE;
	struct FSwapchainImage { void* D3D12Resource = nullptr; };
	TArray<FSwapchainImage> SwapchainImages;
	TArray<FTextureRHIRef> SwapchainImagesRHI;
	uint32 SwapchainWidth = 0;
	uint32 SwapchainHeight = 0;
	int64 SwapchainFormat = 0;

	// --- IPC array path ---
	// The single-tiled arraySize=1 shared texture is non-coherent cross-process
	// from UE's process (proven: even a dedicated device's writes don't reach the
	// D3D11 service). The canonical stereo arraySize=2 swapchain IS coherent
	// (proven on UE's own device). So over IPC we render UE's two eyes SBS into a
	// private RT, then CopyTexture each eye into an arraySize=2 slice and submit
	// that. In-process keeps the v0.4.3 single-tiled zero-copy path unchanged.
	bool bUseCopyPath = false;                 // XRT_FORCE_MODE=ipc / DISPLAYXR_WORKSPACE_SESSION
	bool bWorkspaceSession = false;            // DISPLAYXR_WORKSPACE_SESSION (shell) — runtime owns the overlay size
	TArray<FTextureRHIRef> ArraySwapchainRHI;  // wrapped imported arraySize=2 images (copy DEST)
	// Per-view (per-slice) dims for the IPC array swapchain. MUST equal the tile
	// dims (content fills the slice, like Unity) — an oversized slice trips the
	// service's needs_scale → workspace scale-blit path which flips V (UE-only
	// upside-down under the shell). The private RT stays display-sized for SBS.
	uint32 SliceW = 0;
	uint32 SliceH = 0;

	// Compositor thread
	FRunnableThread* Thread = nullptr;
	TAtomic<bool> bThreadRunning{false};
	TAtomic<bool> bStopRequested{false};

	// Per-frame handshake
	FEvent* BeginFrameReadyEvent = nullptr;   // compositor → game (manual-reset)
	FEvent* EndFrameReadyEvent = nullptr;     // render → compositor (auto-reset)
	TAtomic<uint32> AcquiredImageIndex{~0u};
	TAtomic<bool> bImageAcquiredThisFrame{false};
	TAtomic<int64> LastPredictedDisplayTime{0};

	// Frame stats
	int32 FrameCount = 0;

	// Cached OpenXR function pointers
	PFN_xrGetInstanceProcAddr xrGetInstanceProcAddrFunc = nullptr;

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

	// Optional extension: DisplayXR runtime's D3D12 native compositor uses
	// this to route its swapchain present into a specific rect of the bound
	// HWND. Called from Tick when OverrideCompositorHWND is set; stays null
	// on runtimes without the extension (game-mode fullscreen unaffected).
	PFN_xrSetSharedTextureOutputRectEXT xrSetOutputRectFunc = nullptr;
	uint32 LastCanvasW = 0;
	uint32 LastCanvasH = 0;
};
