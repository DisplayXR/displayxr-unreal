// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRCompositor.h"
#include "DisplayXRPlatform.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"
#include "ID3D12DynamicRHI.h"
#include "RenderingThread.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRCompositor, Log, All);

#define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ((XrStructureType)1000028001)
struct XrSwapchainImageD3D12KHR { XrStructureType type; void* next; void* texture; };

// =============================================================================
// Child window
// =============================================================================
#if PLATFORM_WINDOWS
static const wchar_t* OVERLAY_CLASS = L"DisplayXROverlay";
static bool bClassReg = false;

// Camera-look drag state for OverlayProc (game thread only). The runtime forwards
// WM_MOUSEMOVE with wParam=0 (no MK_* bits — it synthesizes moves from the shell's
// IPC cursor stream), so we latch the held state from the button events instead.
static bool sOverlayBtnHeld = false;
static bool sOverlayTracking = false;
static int sOverlayLastX = 0;
static int sOverlayLastY = 0;

// Drive camera-look straight on the local PlayerController. Under the shell UE's
// window is never OS-foreground and the cursor lives over the shell window, so
// Slate routes mouse input (raw OR cursor-based) AWAY from UE's viewport — the
// look axis never sees it no matter how we inject at the input layer. Skip that
// layer entirely: AddYaw/PitchInput accumulates onto the controller's rotation
// input the same way the game's own look mapping does (Pawn::AddControllerYawInput
// forwards here), so it works regardless of focus/capture/cursor position. Game
// thread only (OverlayProc runs in UE's message pump). Scale = degrees per pixel.
static void DisplayXRFeedLook(int dx, int dy)
{
	if (!GEngine || !GEngine->GameViewport) return;
	UWorld* W = GEngine->GameViewport->GetWorld();
	if (!W) return;
	APlayerController* PC = W->GetFirstPlayerController();
	if (!PC) return;
	const float kScale = 0.15f;
	if (dx != 0) PC->AddYawInput(dx * kScale);
	if (dy != 0) PC->AddPitchInput(dy * kScale);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	// The runtime forwards focused-app input (keyboard/mouse) via PostMessage to
	// the HWND bound through XR_EXT_win32_window_binding — which for us is this
	// inert child overlay. Relay that input to UE's REAL window (our parent) so
	// WASD/mouse actually drive the app; otherwise every forwarded key/click hits
	// DefWindowProcW and is dropped (native apps that bind their real window work).
	switch (m) {
	case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
	case WM_CHAR: case WM_SYSCHAR: {
		HWND tgt = GetParent(h);
		// Strip the runtime's modifier-marker bits (25-28, reserved in WM_KEY*
		// lParam) so UE sees a clean message.
		if (tgt) PostMessageW(tgt, m, w, l & ~(LPARAM)(0xFu << 25));
		return 0;
	}
	case WM_MOUSEMOVE: {
		// While a drag is held (the shell's "left-drag = app input" gesture), feed
		// the frame-to-frame delta to the camera. wParam has NO button bits here, so
		// held comes from the latched button events below; seed on the first move.
		const int mx = (int)(int16_t)(l & 0xFFFF);          // GET_X_LPARAM, no <windowsx.h>
		const int my = (int)(int16_t)((l >> 16) & 0xFFFF);  // GET_Y_LPARAM (signed)
		if (sOverlayBtnHeld) {
			if (sOverlayTracking) DisplayXRFeedLook(mx - sOverlayLastX, my - sOverlayLastY);
			sOverlayLastX = mx; sOverlayLastY = my; sOverlayTracking = true;
		}
		HWND tgt = GetParent(h);
		if (tgt) PostMessageW(tgt, m, w, l);
		return 0;
	}
	case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
	case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
		sOverlayBtnHeld = true; sOverlayTracking = false;  // next move seeds the origin
		HWND tgt = GetParent(h);
		if (tgt) PostMessageW(tgt, m, w, l);
		return 0;
	}
	case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
		sOverlayBtnHeld = false; sOverlayTracking = false;
		HWND tgt = GetParent(h);
		if (tgt) PostMessageW(tgt, m, w, l);
		return 0;
	}
	case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
		HWND tgt = GetParent(h);
		if (tgt) PostMessageW(tgt, m, w, l);
		return 0;
	}
	}
	return m == WM_NCHITTEST ? HTTRANSPARENT : DefWindowProcW(h, m, w, l);
}
static bool RegClass() {
	if (bClassReg) return true;
	WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.style = CS_OWNDC;
	wc.lpfnWndProc = OverlayProc; wc.hInstance = GetModuleHandleW(0);
	wc.lpszClassName = OVERLAY_CLASS;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
	bClassReg = true; return true;
}
#endif

// =============================================================================
// Compositor thread runnable
// =============================================================================
class FCompositorRunnable : public FRunnable {
public:
	FCompositorRunnable(FDisplayXRCompositor* O) : Owner(O) {}
	uint32 Run() override { Owner->CompositorLoop(); return 0; }
	FDisplayXRCompositor* Owner;
};

void FDisplayXRCompositor::ComputeTileDims(int32& OutTW, int32& OutTH) const
{
	FDisplayXRViewConfig VC = Session->GetViewConfig();
	OutTW = FMath::Max(1, VC.GetTileW());
	OutTH = FMath::Max(1, VC.GetTileH());
#if PLATFORM_WINDOWS
	// Measure the BOUND window (overlay) — under the shell the runtime sizes it to
	// the workspace window, so this tracks a resize. The device's CacheWindowSize
	// reads the same HWND, so UE's render rect, the copy source rect, and the
	// submitted imageRect all agree and the content aspect tracks the window.
	HWND BoundHWND = (HWND)GetBoundHWND();
	if (BoundHWND)
	{
		RECT pcr = {};
		if (::GetClientRect(BoundHWND, &pcr))
		{
			const int32 BoundW = pcr.right - pcr.left;
			const int32 BoundH = pcr.bottom - pcr.top;
			if (BoundW > 0 && BoundH > 0)
			{
				const int32 ClampedW = FMath::Min<int32>(BoundW, (int32)SwapchainWidth);
				const int32 ClampedH = FMath::Min<int32>(BoundH, (int32)SwapchainHeight);
				OutTW = FMath::Max(1, FMath::RoundToInt(ClampedW * VC.ScaleX));
				OutTH = FMath::Max(1, FMath::RoundToInt(ClampedH * VC.ScaleY));
			}
		}
	}
#endif
}

void FDisplayXRCompositor::CompositorLoop()
{
#if PLATFORM_WINDOWS
	XrSession XrSess = Session->GetXrSession();
	if (XrSess == XR_NULL_HANDLE) return;

	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor Thread: Started"));
	GLog->Flush();

	while (!bStopRequested.Load())
	{
		// Pump lifecycle events (STOPPING, EXITING, focus/visibility) on THIS
		// (compositor) thread, serialized with the frame calls below. FOCUSED was
		// already reached in the session warmup (CreateSessionWithGraphics), so this
		// only ever sees steady-state and cannot trigger the late-FOCUSED handshake
		// livelock. CRITICAL: this MUST run even when the session is parked. On a
		// shell close the runtime drives EXIT_REQUEST → STOPPING; we call
		// xrEndSession (parking the session), which makes the runtime queue
		// EXITING — but EXITING is only delivered on a SUBSEQUENT poll. The old code
		// gated PumpEvents behind IsSessionRunning and parked first, so after
		// STOPPING it never polled again: EXITING was stranded, nothing requested
		// app exit, and the process spun a parked, session-less loop forever (the
		// teardown "hang"). Pump first, unconditionally.
		Session->PumpEvents();

		// Parked (session ended, e.g. post-STOPPING / awaiting EXITING). Wake any
		// game thread blocked in AcquireImage_GameThread (manual-reset
		// BeginFrameReadyEvent) so it bails with -1 instead of crawling at its
		// 500ms timeout while UE shuts down. Keep looping so PumpEvents above can
		// still receive EXITING and request exit. The loop exits when UE's HMD
		// shutdown calls StopCompositorThread (bStopRequested).
		if (!Session->IsSessionRunning())
		{
			if (BeginFrameReadyEvent) BeginFrameReadyEvent->Trigger();
			FPlatformProcess::Sleep(0.01f);
			continue;
		}

		FrameCount++;

		// xrWaitFrame
		XrFrameWaitInfo WI = {XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState FS = {XR_TYPE_FRAME_STATE};
		XrResult r = xrWaitFrameFunc(XrSess, &WI, &FS);
		if (!XR_SUCCEEDED(r)) { FPlatformProcess::Sleep(0.016f); continue; }

		// Share predicted display time with game thread for xrLocateViews.
		// Without this, LocateViews is called with displayTime=0 and returns
		// XR_ERROR_TIME_INVALID, freezing eye tracking.
		Session->SetPredictedDisplayTime((int64)FS.predictedDisplayTime);

		// xrBeginFrame
		XrFrameBeginInfo BI = {XR_TYPE_FRAME_BEGIN_INFO};
		r = xrBeginFrameFunc(XrSess, &BI);
		if (!XR_SUCCEEDED(r)) continue;

		LastPredictedDisplayTime.Store((int64)FS.predictedDisplayTime);

		// Signal game thread that it can now acquire a swapchain image
		if (BeginFrameReadyEvent) BeginFrameReadyEvent->Trigger();

		if (!FS.shouldRender || !bSwapchainCreated) {
			XrFrameEndInfo EI = {XR_TYPE_FRAME_END_INFO};
			EI.displayTime = FS.predictedDisplayTime;
			EI.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			xrEndFrameFunc(XrSess, &EI);
			if (BeginFrameReadyEvent) BeginFrameReadyEvent->Reset();
			continue;
		}

		// Wait for UE render thread to release the swapchain image.
		// Long timeout: UE's render thread may take ≥ vsync interval to finish.
		// IMPORTANT: we do NOT clear bImageAcquiredThisFrame here — that's
		// owned exclusively by Release on the render thread. Clearing it from
		// here would cause Release to skip, leaking the image forever.
		const bool bGotImage = EndFrameReadyEvent && EndFrameReadyEvent->Wait(500);

		if (!bGotImage) {
			// UE didn't release in time — submit an empty frame and loop.
			XrFrameEndInfo EI = {XR_TYPE_FRAME_END_INFO};
			EI.displayTime = FS.predictedDisplayTime;
			EI.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			xrEndFrameFunc(XrSess, &EI);
			if (BeginFrameReadyEvent) BeginFrameReadyEvent->Reset();
			continue;
		}

		// Build projection layer.
		//
		// CRITICAL: the imageRect we declare here is what the DisplayXR runtime
		// reads from the atlas to composite onto the physical panel (see reference
		// cube_handle_d3d12_win/main.cpp:482-488). It MUST match where UE's
		// AdjustViewRect told UE to write each tile — otherwise the runtime's
		// sample rect straddles two of UE's tiles and one eye sees the other's
		// content (right-eye-in-left-eye bleed).
		//
		// HWND identity: we bind ChildHWND via XR_EXT_win32_window_binding, so
		// the runtime calls GetClientRect(ChildHWND). ChildHWND is created at a
		// fixed size at session-init and does NOT auto-track the parent resize
		// — we have to sync it explicitly here each frame so the runtime sees
		// the current window size, and so that both sides agree on tile dims.
		FDisplayXRViewConfig VC = Session->GetViewConfig();
		int32 NV = VC.GetViewCount();
		// Window-relative tile dims (both paths) — the SAME helper the copy uses,
		// so UE's render rect, the copy source, and this imageRect all agree and
		// the content aspect tracks the window (correct under resize).
		int32 TW, TH;
		ComputeTileDims(TW, TH);
#if PLATFORM_WINDOWS
		// In-process / forced-IPC: keep ChildHWND sized to UE's main window so the
		// runtime's GetClientRect(ChildHWND) sees it. Under the SHELL we do NOT do
		// this — the runtime sizes the overlay to the workspace window (it owns the
		// size), and we measure that (GetBoundHWND) so content tracks the resize.
		if (!bWorkspaceSession && ParentHWND && ChildHWND)
		{
			RECT pcr = {};
			if (::GetClientRect((HWND)ParentHWND, &pcr))
			{
				const int32 ParentW = pcr.right - pcr.left;
				const int32 ParentH = pcr.bottom - pcr.top;
				RECT ccr = {};
				::GetClientRect((HWND)ChildHWND, &ccr);
				if (ParentW > 0 && ParentH > 0 &&
					(ParentW != (ccr.right - ccr.left) || ParentH != (ccr.bottom - ccr.top)))
				{
					::SetWindowPos((HWND)ChildHWND, nullptr, 0, 0, ParentW, ParentH,
						SWP_NOZORDER | SWP_NOACTIVATE);
				}
			}
		}
		// Under the shell, UE's own top-level window would otherwise show its
		// fullscreen mono mirror on the desktop, over the shell. Hide it WITHOUT
		// changing its style, size, or position — every other approach perturbs the
		// 3D content:
		//   - off-screen park / shrink drags or clips the WS_CHILD overlay the
		//     runtime measures -> DISTORTED content;
		//   - WS_EX_LAYERED (transparent) breaks UE's flip-model present to the
		//     window, UE falls back to a differently-aligned render -> WRONG POV
		//     (camera too low). (Confirmed: hidden-via-layered <-> POV shifted.)
		// Instead clip the window to an EMPTY region: the window rect stays
		// full-size at its origin (overlay geometry + runtime Kooima untouched, the
		// swapchain present is NOT redirected) but nothing composites to the
		// desktop. UE keeps rendering its scene (which the plugin copies into the
		// array swapchain) — paired with t.IdleWhenNotForeground 0. Applied once.
		else if (bWorkspaceSession && ParentHWND && !bWorkspaceWindowHidden)
		{
			::SetWindowRgn((HWND)ParentHWND, ::CreateRectRgn(0, 0, 0, 0), true);
			bWorkspaceWindowHidden = true;
		}
#endif
		int32 Cols = FMath::Max(VC.TileColumns, 1);

		FVector LE, RE; bool bT;
		Session->GetEyePositions(LE, RE, bT);

		TArray<XrCompositionLayerProjectionView> PV;
		PV.SetNum(NV);
		for (int32 i = 0; i < NV; i++) {
			PV[i] = {}; PV[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			// Submit the runtime's located pose + off-axis (Kooima) fov from
			// xrLocateViews (matches cube_handle_d3d12_win). A hardcoded fov is
			// off-center / wrong-aspect. Fall back only if locate data is missing.
			FVector P; FQuat Q; XrFovf F;
			if (Session->GetViewData(i, P, Q, F)) {
				PV[i].pose.position = {(float)P.X, (float)P.Y, (float)P.Z};
				PV[i].pose.orientation = {(float)Q.X, (float)Q.Y, (float)Q.Z, (float)Q.W};
				PV[i].fov = F;
			} else {
				FVector E = (i == 0) ? LE : RE;
				PV[i].pose.position = {(float)E.X, (float)E.Y, (float)E.Z};
				PV[i].pose.orientation = {0,0,0,1};
				PV[i].fov = {-0.5f, 0.5f, 0.3f, -0.3f};
			}
			PV[i].subImage.swapchain = Swapchain;
			if (bUseCopyPath) {
				// IPC array: each eye is its own slice; content copied to slice origin.
				PV[i].subImage.imageArrayIndex = (uint32_t)i;
				PV[i].subImage.imageRect.offset = {0, 0};
				PV[i].subImage.imageRect.extent = {TW, TH};
			} else {
				PV[i].subImage.imageRect.offset = {(i % Cols) * TW, (i / Cols) * TH};
				PV[i].subImage.imageRect.extent = {TW, TH};
			}
		}

		XrCompositionLayerProjection PL = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		PL.space = Session->GetXrSpace();
		PL.viewCount = (uint32_t)NV;
		PL.views = PV.GetData();
		const XrCompositionLayerBaseHeader* Layers[] = {(const XrCompositionLayerBaseHeader*)&PL};

		XrFrameEndInfo EI = {XR_TYPE_FRAME_END_INFO};
		EI.displayTime = FS.predictedDisplayTime;
		EI.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		EI.layerCount = 1;
		EI.layers = Layers;
		r = xrEndFrameFunc(XrSess, &EI);

		if (BeginFrameReadyEvent) BeginFrameReadyEvent->Reset();

		static bool bFirst = true;
		if (bFirst) {
			bFirst = false;
			UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor Thread: First atlas frame (frame %d, %d views %dx%d, xrEndFrame=%d)"),
				FrameCount, NV, TW, TH, (int)r);
			GLog->Flush();
		} else if (FrameCount % 300 == 0) {
			UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor Thread: Frame %d"), FrameCount);
		}
	}
	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor Thread: Stopped"));
	GLog->Flush();
#endif
}

// =============================================================================
// Constructor / Destructor
// =============================================================================
FDisplayXRCompositor::FDisplayXRCompositor(FDisplayXRSession* S) : Session(S) {}
FDisplayXRCompositor::~FDisplayXRCompositor() { Shutdown(); }

// =============================================================================
// Initialize
// =============================================================================
bool FDisplayXRCompositor::Initialize(void* InParentHWND, void* InD3DDevice, void* InCommandQueue)
{
	UEDevice = InD3DDevice;
	UECommandQueue = InCommandQueue;
	if (!UEDevice || !InParentHWND || !Session) return false;
	if (!ResolveXrFunctions()) return false;

	// Create a dedicated runtime queue on UE's device.
	// DO NOT pass UE's main queue — the runtime may submit its own work which
	// would corrupt UE's RHI command-list tracking.
	if (!CreateRuntimeQueue()) return false;

	// IPC vs in-process: over IPC the single-tiled shared texture is non-coherent
	// cross-process from UE's process, so we take the arraySize=2 copy path.
	{
		const FString ForceMode = FPlatformMisc::GetEnvironmentVariable(TEXT("XRT_FORCE_MODE"));
		const FString Workspace = FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAYXR_WORKSPACE_SESSION"));
		bWorkspaceSession = !Workspace.IsEmpty();
		bUseCopyPath = ForceMode.Equals(TEXT("ipc"), ESearchCase::IgnoreCase) || bWorkspaceSession;
		UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: swapchain path = %s"),
			bUseCopyPath ? TEXT("IPC array copy (arraySize=2 slice/eye)") : TEXT("in-process single-tiled zero-copy"));
	}

	// Under the shell UE's top-level window is never the foreground window (the
	// shell's workspace window is), so by default UE throttles its render loop to
	// a few fps when it loses focus — which starves the compositor thread of real
	// frames. Keep it rendering at full rate: UE's equivalent of Unity's "Run in
	// Background". Paired with the off-screen window park in CompositorLoop so the
	// still-rendering window shows nothing on the desktop.
	if (bWorkspaceSession)
	{
		if (IConsoleVariable* CVarIdle =
		        IConsoleManager::Get().FindConsoleVariable(TEXT("t.IdleWhenNotForeground")))
		{
			CVarIdle->Set(0, ECVF_SetByCode);
		}
	}

	// Child-window strategy:
	//  - Game mode (OverrideCompositorHWND null): keep the historical child
	//    window. The fullscreen game window has no visible pixels behind it
	//    so a WS_CHILD overlay is harmless.
	//  - Editor native-PIE (OverrideCompositorHWND set): the override HWND is
	//    our own raw-Win32 top-level mirror. Skip the child and bind the
	//    session directly — matches the shipped FDisplayXRPreviewSession
	//    pattern and keeps the DWM composite simple.
	const bool bUseParentDirectly = (FDisplayXRPlatform::OverrideCompositorHWND != nullptr);
	ParentHWND = InParentHWND;  // track either way so Tick's rect call has an HWND
	if (!bUseParentDirectly)
	{
		if (!CreateChildWindow(InParentHWND)) return false;
	}
	void* const SessionHWND = bUseParentDirectly ? InParentHWND : ChildHWND;

	// Create session with UE's device + our dedicated runtime queue + bound window
	if (!Session->IsSessionCreated()) {
		if (!Session->CreateSessionWithGraphics(UEDevice, RuntimeQueue, SessionHWND)) {
			UE_LOG(LogDisplayXRCompositor, Error, TEXT("Compositor: Session creation failed"));
			return false;
		}
	}

	BeginFrameReadyEvent = FPlatformProcess::GetSynchEventFromPool(true /*manualReset*/);
	EndFrameReadyEvent = FPlatformProcess::GetSynchEventFromPool(false /*autoReset*/);

	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Initialized (single-device, zero-copy mode; swapchain deferred)"));
	GLog->Flush();
	return true;
}

void FDisplayXRCompositor::Shutdown()
{
	StopCompositorThread();
	SwapchainImagesRHI.Empty();
	ArraySwapchainRHI.Empty();
	DestroySwapchain();
	DestroyChildWindow();
#if PLATFORM_WINDOWS
	if (RuntimeQueue) { static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release(); RuntimeQueue = nullptr; }
#endif
	if (BeginFrameReadyEvent) { FPlatformProcess::ReturnSynchEventToPool(BeginFrameReadyEvent); BeginFrameReadyEvent = nullptr; }
	if (EndFrameReadyEvent) { FPlatformProcess::ReturnSynchEventToPool(EndFrameReadyEvent); EndFrameReadyEvent = nullptr; }
	bReady = false;
}

// =============================================================================
// Tick
// =============================================================================
void FDisplayXRCompositor::Tick()
{
	if (!bReady) {
		if (Session && Session->IsSessionRunning() && !bSwapchainCreated) {
			UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Creating swapchain..."));
			GLog->Flush();
			if (CreateSwapchain() && WrapSwapchainImagesAsRHI()) {
				if (StartCompositorThread()) {
					bReady = true;
					UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Ready"));
					GLog->Flush();
				}
			}
		}
		return;
	}

#if PLATFORM_WINDOWS
	// Gated on OverrideCompositorHWND so game mode — which has never called
	// this function — stays silent. When the override is set (editor native-
	// PIE), push the bound HWND's client rect to the runtime so its native
	// compositor knows where to present the atlas. Shipped preview calls
	// this every tick; logging is throttled to size changes.
	if (xrSetOutputRectFunc && Session && Session->IsSessionRunning()
		&& FDisplayXRPlatform::OverrideCompositorHWND != nullptr)
	{
		HWND BoundHWND = (HWND)(ChildHWND ? ChildHWND : ParentHWND);
		RECT rc;
		if (BoundHWND && GetClientRect(BoundHWND, &rc))
		{
			const uint32 W = (uint32)(rc.right - rc.left);
			const uint32 H = (uint32)(rc.bottom - rc.top);
			if (W > 0 && H > 0)
			{
				xrSetOutputRectFunc(Session->GetXrSession(), 0, 0, W, H);
				if (W != LastCanvasW || H != LastCanvasH)
				{
					UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: output rect updated %ux%u"), W, H);
					LastCanvasW = W;
					LastCanvasH = H;
				}
			}
		}
	}
#endif
}

// =============================================================================
// Runtime queue (on UE's device, dedicated for OpenXR session)
// =============================================================================
bool FDisplayXRCompositor::CreateRuntimeQueue()
{
#if PLATFORM_WINDOWS
	ID3D12Device* UEDev = static_cast<ID3D12Device*>(UEDevice);
	if (!UEDev) return false;

	D3D12_COMMAND_QUEUE_DESC QD = {};
	QD.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	QD.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	QD.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	QD.NodeMask = 0;

	HRESULT hr = UEDev->CreateCommandQueue(&QD,
		IID_PPV_ARGS(reinterpret_cast<ID3D12CommandQueue**>(&RuntimeQueue)));
	if (FAILED(hr) || !RuntimeQueue) {
		UE_LOG(LogDisplayXRCompositor, Error, TEXT("Compositor: RuntimeQueue creation failed (0x%08x)"), (unsigned)hr);
		return false;
	}

	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: RuntimeQueue created on UE device (%p)"), RuntimeQueue);
	GLog->Flush();
	return true;
#else
	return false;
#endif
}

// =============================================================================
// Wrap swapchain images as FRHITexture
// =============================================================================
bool FDisplayXRCompositor::WrapSwapchainImagesAsRHI()
{
#if PLATFORM_WINDOWS
	if (SwapchainImages.Num() == 0) return false;

	ID3D12DynamicRHI* DynamicRHI = GetID3D12DynamicRHI();
	if (!DynamicRHI) {
		UE_LOG(LogDisplayXRCompositor, Error, TEXT("Compositor: GetID3D12DynamicRHI returned null"));
		return false;
	}

	// IPC array path: UE renders both eyes SBS into a PRIVATE RT (display-sized,
	// so the existing AdjustViewRect tiling is unchanged); we later CopyTexture
	// each eye into an arraySize=2 slice of the imported image. Wrap the imported
	// array images separately as the copy DEST.
	if (bUseCopyPath)
	{
		SwapchainImagesRHI.Empty(SwapchainImages.Num());
		ArraySwapchainRHI.Empty(SwapchainImages.Num());
		const uint32 W = SwapchainWidth, H = SwapchainHeight;
		TArray<FTextureRHIRef>& OutPrivate = SwapchainImagesRHI;
		TArray<FTextureRHIRef>& OutArray   = ArraySwapchainRHI;
		TArray<FSwapchainImage> InImages = SwapchainImages;
		ENQUEUE_RENDER_COMMAND(WrapDisplayXRArraySwapchain)(
			[&OutPrivate, &OutArray, InImages, DynamicRHI, W, H](FRHICommandListImmediate& RHICmdList)
			{
				for (const FSwapchainImage& Img : InImages)
				{
					const FRHITextureCreateDesc Desc =
						FRHITextureCreateDesc::Create2D(TEXT("DisplayXRPrivateRT"), (int32)W, (int32)H, PF_R8G8B8A8)
							.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
							.SetClearValue(FClearValueBinding::Black)
							.SetInitialState(ERHIAccess::SRVMask);
					OutPrivate.Add(RHICmdList.CreateTexture(Desc));

					ID3D12Resource* Res = static_cast<ID3D12Resource*>(Img.D3D12Resource);
					OutArray.Add(DynamicRHI->RHICreateTexture2DArrayFromResource(
						PF_R8G8B8A8,
						ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource,
						FClearValueBinding::Transparent, Res));
				}
			});
		FlushRenderingCommands();
		UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: IPC array path — %d private RTs + %d array images"),
			SwapchainImagesRHI.Num(), ArraySwapchainRHI.Num());
		GLog->Flush();
		return SwapchainImagesRHI.Num() > 0 && ArraySwapchainRHI.Num() == SwapchainImagesRHI.Num();
	}

	SwapchainImagesRHI.Empty(SwapchainImages.Num());

	const ETextureCreateFlags Flags =
		ETextureCreateFlags::RenderTargetable |
		ETextureCreateFlags::ShaderResource |
		ETextureCreateFlags::ResolveTargetable |
		ETextureCreateFlags::Dynamic;

	// RHICreateTexture2DFromResource must run on the render thread.
	TArray<FTextureRHIRef>& OutImages = SwapchainImagesRHI;
	TArray<FSwapchainImage> InImages = SwapchainImages;
	ENQUEUE_RENDER_COMMAND(WrapDisplayXRSwapchainImages)(
		[&OutImages, InImages, DynamicRHI, Flags](FRHICommandListImmediate& RHICmdList)
		{
			for (const FSwapchainImage& Img : InImages)
			{
				ID3D12Resource* Res = static_cast<ID3D12Resource*>(Img.D3D12Resource);
				FTextureRHIRef Wrapped = DynamicRHI->RHICreateTexture2DFromResource(
					PF_B8G8R8A8, Flags, FClearValueBinding::Transparent, Res);
				OutImages.Add(Wrapped);
			}
		});
	FlushRenderingCommands();

	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Wrapped %d swapchain images as RHI"),
		SwapchainImagesRHI.Num());
	GLog->Flush();
	return SwapchainImagesRHI.Num() > 0;
#else
	return false;
#endif
}

// =============================================================================
// UE render target integration
// =============================================================================
bool FDisplayXRCompositor::GetSwapchainImagesRHI(TArray<FTextureRHIRef>& OutImages) const
{
	if (SwapchainImagesRHI.Num() == 0) return false;
	OutImages = SwapchainImagesRHI;
	return true;
}

int32 FDisplayXRCompositor::AcquireImage_GameThread()
{
#if PLATFORM_WINDOWS
	static int32 CallCount = 0;
	CallCount++;
	const int32 LocalCallCount = CallCount;

	if (Swapchain == XR_NULL_HANDLE || !xrAcquireSwapchainImageFunc || !xrWaitSwapchainImageFunc)
	{
		if (LocalCallCount <= 5)
			UE_LOG(LogDisplayXRCompositor, Warning, TEXT("AcquireColorTexture #%d: no swapchain"), LocalCallCount);
		return -1;
	}

	// Defensive: if we have an image still acquired from a prior call (e.g.
	// render thread didn't run PostRenderViewFamily yet), return the same idx
	// rather than calling xrAcquire twice. This prevents leaking swapchain
	// images and exhausting the pool.
	if (bImageAcquiredThisFrame.Load())
	{
		const int32 Cached = (int32)AcquiredImageIndex.Load();
		if (LocalCallCount <= 20 || LocalCallCount % 300 == 0)
		{
			UE_LOG(LogDisplayXRCompositor, Warning, TEXT("AcquireColorTexture #%d: reusing idx=%d (prior release not yet run)"), LocalCallCount, Cached);
			GLog->Flush();
		}
		return Cached;
	}

	// Wait for compositor thread to have called xrBeginFrame for this frame
	if (BeginFrameReadyEvent && !BeginFrameReadyEvent->Wait(500))
	{
		if (LocalCallCount <= 20 || LocalCallCount % 300 == 0)
		{
			UE_LOG(LogDisplayXRCompositor, Warning, TEXT("AcquireColorTexture #%d: BeginFrameReady timeout"), LocalCallCount);
			GLog->Flush();
		}
		return -1;
	}

	// Session parked / stopping (e.g. the shell closed the app, driving STOPPING).
	// The compositor thread wakes us via BeginFrameReadyEvent on park, but there is
	// no live session to acquire from — return no-image so UE completes the frame
	// and proceeds to shut down, rather than acquiring/waiting on a dead swapchain
	// (which would crawl at the 500ms timeout and look hung).
	if (bStopRequested.Load() || (Session && !Session->IsSessionRunning()))
	{
		return -1;
	}

	uint32_t Idx = 0;
	XrSwapchainImageAcquireInfo AI = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	XrResult r = xrAcquireSwapchainImageFunc(Swapchain, &AI, &Idx);
	if (!XR_SUCCEEDED(r))
	{
		if (LocalCallCount <= 20)
			UE_LOG(LogDisplayXRCompositor, Warning, TEXT("AcquireColorTexture #%d: xrAcquireSwapchainImage failed %d"), LocalCallCount, (int)r);
		return -1;
	}

	XrSwapchainImageWaitInfo SWI = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	SWI.timeout = 100 * 1000000LL; // 100 ms
	r = xrWaitSwapchainImageFunc(Swapchain, &SWI);
	if (!XR_SUCCEEDED(r))
	{
		if (LocalCallCount <= 20)
			UE_LOG(LogDisplayXRCompositor, Warning, TEXT("AcquireColorTexture #%d: xrWaitSwapchainImage failed %d"), LocalCallCount, (int)r);
		return -1;
	}

	AcquiredImageIndex.Store(Idx);
	bImageAcquiredThisFrame.Store(true);

	if (LocalCallCount <= 10 || LocalCallCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRCompositor, Log, TEXT("AcquireColorTexture #%d -> idx=%u"), LocalCallCount, Idx);
		GLog->Flush();
	}

	return (int32)Idx;
#else
	return -1;
#endif
}

void FDisplayXRCompositor::ReleaseImage_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* SwapchainTexture)
{
#if PLATFORM_WINDOWS
	static int32 CallCount = 0;
	CallCount++;
	const int32 LocalCallCount = CallCount;

	// Atomically claim the release. If bImageAcquiredThisFrame was already
	// false, or another thread already claimed it, skip.
	bool Expected = true;
	if (!bImageAcquiredThisFrame.CompareExchange(Expected, false))
	{
		if (LocalCallCount <= 20 || LocalCallCount % 300 == 0)
		{
			UE_LOG(LogDisplayXRCompositor, Log, TEXT("ReleaseImage_RT #%d: no image acquired"), LocalCallCount);
			GLog->Flush();
		}
		return;
	}
	if (Swapchain == XR_NULL_HANDLE || !xrReleaseSwapchainImageFunc) return;

	if (bUseCopyPath)
	{
		// IPC: UE rendered both eyes SBS into the private RT (SwapchainTexture).
		// Copy each eye's tile into its arraySize=2 slice of the imported image,
		// then drain so the writes are GPU-complete (cross-process coherence)
		// before xrReleaseSwapchainImage.
		const uint32 Idx = AcquiredImageIndex.Load();
		if (SwapchainTexture && Idx < (uint32)ArraySwapchainRHI.Num() && ArraySwapchainRHI[Idx].IsValid())
		{
			FRHITexture* Dst = ArraySwapchainRHI[Idx].GetReference();
			FDisplayXRViewConfig VC = Session->GetViewConfig();
			const int32 NV = VC.GetViewCount();
			// Window-relative — MUST match UE's AdjustViewRect render rect and the
			// projection imageRect (same helper, same window), else the copy reads
			// a different region than UE drew (black band / shifted tile).
			int32 TW, TH;
			ComputeTileDims(TW, TH);
			const int32 Cols = FMath::Max(VC.TileColumns, 1);

			RHICmdList.Transition(FRHITransitionInfo(SwapchainTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
			RHICmdList.Transition(FRHITransitionInfo(Dst, ERHIAccess::Unknown, ERHIAccess::CopyDest));
			for (int32 i = 0; i < NV; i++)
			{
				FRHICopyTextureInfo CopyInfo;
				CopyInfo.Size = FIntVector(TW, TH, 1);
				CopyInfo.SourcePosition = FIntVector((i % Cols) * TW, (i / Cols) * TH, 0);
				CopyInfo.DestPosition = FIntVector(0, 0, 0);
				CopyInfo.DestSliceIndex = (uint32)i;
				CopyInfo.NumSlices = 1;
				RHICmdList.CopyTexture(SwapchainTexture, Dst, CopyInfo);
			}
			RHICmdList.Transition(FRHITransitionInfo(Dst, ERHIAccess::CopyDest, ERHIAccess::Present));
			// Flush so the copy is queued before xrReleaseSwapchainImage; rely on
			// the runtime's swapchain-release GPU sync rather than a full CPU stall
			// (BlockUntilGPUIdle halved the framerate). If the service ever reads a
			// torn/incomplete slice, reinstate a *targeted* fence wait here.
			RHICmdList.SubmitCommandsHint();
		}
	}
	else if (SwapchainTexture)
	{
		// In-process: UE rendered directly into the swapchain image (zero-copy).
		RHICmdList.Transition(FRHITransitionInfo(SwapchainTexture, ERHIAccess::Unknown, ERHIAccess::Present));
		RHICmdList.SubmitCommandsHint();
	}

	XrSwapchainImageReleaseInfo RI = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	XrResult r = xrReleaseSwapchainImageFunc(Swapchain, &RI);

	if (LocalCallCount <= 10 || LocalCallCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRCompositor, Log, TEXT("ReleaseImage_RT #%d (xr=%d)"), LocalCallCount, (int)r);
		GLog->Flush();
	}

	if (EndFrameReadyEvent) EndFrameReadyEvent->Trigger();
#endif
}

// =============================================================================
// Thread management
// =============================================================================
bool FDisplayXRCompositor::StartCompositorThread()
{
	bStopRequested.Store(false);
	bThreadRunning.Store(true);
	Thread = FRunnableThread::Create(new FCompositorRunnable(this), TEXT("DisplayXRCompositor"), 0, TPri_AboveNormal);
	if (!Thread) { bThreadRunning.Store(false); return false; }
	return true;
}

void FDisplayXRCompositor::StopCompositorThread()
{
	if (!bThreadRunning.Load()) return;
	bStopRequested.Store(true);
	if (BeginFrameReadyEvent) BeginFrameReadyEvent->Trigger();
	if (EndFrameReadyEvent) EndFrameReadyEvent->Trigger();
	if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
	bThreadRunning.Store(false);
}

// =============================================================================
// Resolve XR functions
// =============================================================================
bool FDisplayXRCompositor::ResolveXrFunctions()
{
	xrGetInstanceProcAddrFunc = Session->GetXrGetInstanceProcAddr();
	XrInstance Inst = Session->GetXrInstance();
	if (!xrGetInstanceProcAddrFunc || Inst == XR_NULL_HANDLE) return false;

	auto R = [&](const char* N, PFN_xrVoidFunction* F) {
		return XR_SUCCEEDED(xrGetInstanceProcAddrFunc(Inst, N, F)) && *F;
	};
	bool ok = true;
	ok &= R("xrWaitFrame", (PFN_xrVoidFunction*)&xrWaitFrameFunc);
	ok &= R("xrBeginFrame", (PFN_xrVoidFunction*)&xrBeginFrameFunc);
	ok &= R("xrEndFrame", (PFN_xrVoidFunction*)&xrEndFrameFunc);
	ok &= R("xrEnumerateSwapchainFormats", (PFN_xrVoidFunction*)&xrEnumerateSwapchainFormatsFunc);
	ok &= R("xrCreateSwapchain", (PFN_xrVoidFunction*)&xrCreateSwapchainFunc);
	ok &= R("xrDestroySwapchain", (PFN_xrVoidFunction*)&xrDestroySwapchainFunc);
	ok &= R("xrEnumerateSwapchainImages", (PFN_xrVoidFunction*)&xrEnumerateSwapchainImagesFunc);
	ok &= R("xrAcquireSwapchainImage", (PFN_xrVoidFunction*)&xrAcquireSwapchainImageFunc);
	ok &= R("xrWaitSwapchainImage", (PFN_xrVoidFunction*)&xrWaitSwapchainImageFunc);
	ok &= R("xrReleaseSwapchainImage", (PFN_xrVoidFunction*)&xrReleaseSwapchainImageFunc);

	// Optional — only present on DisplayXR runtimes with the shared-texture
	// output-rect extension. Not gated on `ok`: game-mode fullscreen still
	// works without it.
	xrGetInstanceProcAddrFunc(Inst, "xrSetSharedTextureOutputRectEXT",
		(PFN_xrVoidFunction*)&xrSetOutputRectFunc);
	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: xrSetSharedTextureOutputRectEXT resolved=%s"),
		xrSetOutputRectFunc ? TEXT("yes") : TEXT("no"));

	return ok;
}

// =============================================================================
// Child window
// =============================================================================
bool FDisplayXRCompositor::CreateChildWindow(void* P)
{
#if PLATFORM_WINDOWS
	ParentHWND = P;
	if (!RegClass()) return false;
	RECT rc; GetClientRect((HWND)P, &rc);
	int W = rc.right, H = rc.bottom;
	// No WS_EX_TRANSPARENT: in game mode there's nothing behind to bleed
	// through anyway, and removing the flag keeps the child opaque if the
	// game-mode path is ever used with a partially-visible parent window.
	ChildHWND = CreateWindowExW(0, OVERLAY_CLASS, L"DisplayXR",
		WS_CHILD | WS_VISIBLE, 0, 0, W, H, (HWND)P, 0, GetModuleHandleW(0), 0);
	if (!ChildHWND) return false;
	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Child window %dx%d"), W, H);
	return true;
#else
	return false;
#endif
}
void FDisplayXRCompositor::DestroyChildWindow()
{
#if PLATFORM_WINDOWS
	if (ChildHWND) { DestroyWindow((HWND)ChildHWND); ChildHWND = nullptr; }
#endif
}

// =============================================================================
// Swapchain
// =============================================================================
bool FDisplayXRCompositor::CreateSwapchain()
{
	XrSession S = Session->GetXrSession();
	if (S == XR_NULL_HANDLE || !xrCreateSwapchainFunc) return false;

	// Swapchain must be at FULL display resolution (not atlas size).
	// The runtime's compositor expects this — tiles are sub-images within it.
	FDisplayXRDisplayInfo DI = Session->GetDisplayInfo();
	SwapchainWidth = DI.DisplayPixelWidth > 0 ? (uint32)DI.DisplayPixelWidth : 3840;
	SwapchainHeight = DI.DisplayPixelHeight > 0 ? (uint32)DI.DisplayPixelHeight : 2160;
	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Display info: %dx%d → swapchain %dx%d"),
		DI.DisplayPixelWidth, DI.DisplayPixelHeight, SwapchainWidth, SwapchainHeight);
	GLog->Flush();
	if (!SwapchainWidth || !SwapchainHeight) return false;

	uint32_t FC = 0;
	xrEnumerateSwapchainFormatsFunc(S, 0, &FC, nullptr);
	if (!FC) return false;
	TArray<int64_t> Fmts; Fmts.SetNum(FC);
	xrEnumerateSwapchainFormatsFunc(S, FC, &FC, Fmts.GetData());

	// In-process single-tiled prefers BGRA(87); the IPC array path uses RGBA(28)
	// (proven coherent cross-process) so the private-RT→slice copy is straight.
	if (bUseCopyPath) {
		SwapchainFormat = 28;
		for (auto F : Fmts) { if (F == 28) { SwapchainFormat = 28; break; } }
	} else {
		SwapchainFormat = Fmts[0];
		for (auto F : Fmts) { if (F == 87) { SwapchainFormat = F; break; } if (F == 28) SwapchainFormat = F; }
	}

	// IPC: each array slice is PER-VIEW sized so content fills it (matches Unity).
	// The private RT UE renders into stays display-sized (SwapchainWidth/Height).
	uint32 CreateW = SwapchainWidth, CreateH = SwapchainHeight;
	if (bUseCopyPath) {
		FDisplayXRViewConfig VC = Session->GetViewConfig();
		SliceW = VC.GetTileW() > 0 ? (uint32)VC.GetTileW() : (SwapchainWidth / 2);
		SliceH = VC.GetTileH() > 0 ? (uint32)VC.GetTileH() : (SwapchainHeight / 2);
		CreateW = SliceW; CreateH = SliceH;
		UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: IPC array slice dims %ux%u (private RT %ux%u)"),
			SliceW, SliceH, SwapchainWidth, SwapchainHeight);
	}

	const uint32 ArrSize = bUseCopyPath ? 2u : 1u;
	XrSwapchainCreateInfo CI = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	CI.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	CI.format = SwapchainFormat;
	CI.sampleCount = 1; CI.width = CreateW; CI.height = CreateH;
	CI.faceCount = 1; CI.arraySize = ArrSize; CI.mipCount = 1;

	XrResult r = xrCreateSwapchainFunc(S, &CI, &Swapchain);
	if (!XR_SUCCEEDED(r)) { UE_LOG(LogDisplayXRCompositor, Error, TEXT("xrCreateSwapchain failed %d"), (int)r); return false; }

	uint32_t IC = 0;
	xrEnumerateSwapchainImagesFunc(Swapchain, 0, &IC, nullptr);
	TArray<XrSwapchainImageD3D12KHR> Imgs; Imgs.SetNum(IC);
	for (uint32_t i = 0; i < IC; i++) { Imgs[i] = {}; Imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR; }
	xrEnumerateSwapchainImagesFunc(Swapchain, IC, &IC, (XrSwapchainImageBaseHeader*)Imgs.GetData());

	SwapchainImages.SetNum(IC);
	for (uint32_t i = 0; i < IC; i++) SwapchainImages[i].D3D12Resource = Imgs[i].texture;

	bSwapchainCreated = true;
	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: Swapchain %dx%d fmt=%lld (%d images)"),
		SwapchainWidth, SwapchainHeight, (long long)SwapchainFormat, IC);
	GLog->Flush();
	return true;
}

void FDisplayXRCompositor::DestroySwapchain()
{
	if (Swapchain != XR_NULL_HANDLE && xrDestroySwapchainFunc) {
		xrDestroySwapchainFunc(Swapchain); Swapchain = XR_NULL_HANDLE;
	}
	SwapchainImages.Empty(); bSwapchainCreated = false;
}
