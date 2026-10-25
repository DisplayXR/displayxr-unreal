// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

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

#if PLATFORM_WINDOWS
// Release-fence handoff (#62), published on the ID3D12CommandQueue the plug-in
// hands to xrCreateSession (XrGraphicsBindingD3D12KHR::queue) as D3D12 private
// data. The queue is the one object the runtime already holds, so no new
// OpenXR struct is needed. Contract (Docs/DisplayXR/AtlasHandoff.md):
//   DXR_UE_RELEASE_FENCE_GUID        -> IUnknown*: the ID3D12Fence (UE's device,
//                                       D3D12_FENCE_FLAG_SHARED), set once at
//                                       compositor init, before xrCreateSession.
//   DXR_UE_RELEASE_FENCE_VALUE_GUID  -> uint64: the value signalled from UE's
//                                       queue for the image being released, set
//                                       BEFORE each xrReleaseSwapchainImage on
//                                       the releasing thread (0 = nothing
//                                       signalled yet).
// GPU-waiting on (fence, value) orders a reader after UE's last write to the
// released image. PUBLISH ONLY: the plug-in does not wait on it anywhere; the
// enforcing side is the runtime (displayxr-runtime#2362: the hub's ingest
// context GPU-waits the published fence). Until a runtime does, nothing is
// ordered by it and it costs no frame time.
// EXPERIMENTAL: no runtime reads this yet. The fence is
// D3D12_FENCE_FLAG_SHARED, not SHARED_CROSS_ADAPTER, so it serves a reader
// on the SAME adapter only (DIRECT ingress); a cross-adapter reader needs a
// different fence and is out of scope.
// {0caebe5f-b789-4338-b291-859f0f09a631}
static const GUID DXR_UE_RELEASE_FENCE_GUID =
	{0x0caebe5f, 0xb789, 0x4338, {0xb2, 0x91, 0x85, 0x9f, 0x0f, 0x09, 0xa6, 0x31}};
// {8fa02ae8-9d02-431e-ab93-bccd79b601f7}
static const GUID DXR_UE_RELEASE_FENCE_VALUE_GUID =
	{0x8fa02ae8, 0x9d02, 0x431e, {0xab, 0x93, 0xbc, 0xcd, 0x79, 0xb6, 0x01, 0xf7}};
#endif

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
	// the HWND bound through XR_DXR_win32_window_binding — which for us is this
	// inert child overlay. Relay that input to UE's REAL window (our parent) so
	// WASD/mouse actually drive the app; otherwise every forwarded key/click hits
	// DefWindowProcW and is dropped (native apps that bind their real window work).
	switch (m) {
	case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
		// Ctrl flows through to UE like every other key. The shell reserves its own
		// Ctrl chords (Ctrl+L/1/2/3, Ctrl+Shift+O/C, Ctrl+Space) and the runtime now
		// buffers the bare Ctrl prefix until the chord disambiguates (#667), so a
		// shell chord's Ctrl never reaches us — while genuine Ctrl combos the app
		// wants (Ctrl+Z, a held Ctrl) still arrive. This replaces the old blanket
		// Ctrl swallow, which blocked ALL Ctrl input under the shell. REQUIRES a
		// runtime build with the #667 arbitration (runtime ≥ the release noted in
		// CHANGELOG); against an older runtime, shell-chord Ctrl will again drive
		// UE's ADefaultPawn MoveUp.
		HWND tgt = GetParent(h);
		// Strip the runtime's modifier-marker bits (25-28, reserved in WM_KEY*
		// lParam) so UE sees a clean message.
		if (tgt) PostMessageW(tgt, m, w, l & ~(LPARAM)(0xFu << 25));
		return 0;
	}
	case WM_CHAR: case WM_SYSCHAR: {
		HWND tgt = GetParent(h);
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

#if PLATFORM_WINDOWS
		// Hide UE's own top-level window from the desktop (it otherwise shows its
		// fullscreen mono mirror over the shell). Do it HERE — early, before the
		// shouldRender gate below — so it's hidden during UE's load/shader phase too
		// (shouldRender is false then; the old projection-section placement left the
		// black window visible for several seconds). Clip to an EMPTY region: the
		// window stays full-size at its origin (the WS_CHILD overlay geometry +
		// runtime Kooima are untouched and the swapchain present is not redirected)
		// but shows nothing. Then hand OS foreground back to the shell (captured at
		// module load) — UE grabbed it when it showed its window, which otherwise
		// keeps the shell from displaying the app until the user alt-tabs back. UE
		// needs no foreground: input is the runtime forward + AddYawInput and render
		// stays alive via t.IdleWhenNotForeground 0. Applied once.
		if (bWorkspaceSession && ParentHWND && !bWorkspaceWindowHidden)
		{
			::SetWindowRgn((HWND)ParentHWND, ::CreateRectRgn(0, 0, 0, 0), true);
			bWorkspaceWindowHidden = true;
			if (HWND ShellFg = (HWND)FDisplayXRPlatform::SavedShellForegroundHWND)
			{
				if (::IsWindow(ShellFg)) ::SetForegroundWindow(ShellFg);
			}
		}
#endif

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
		// HWND identity: we bind ChildHWND via XR_DXR_win32_window_binding, so
		// the runtime calls GetClientRect(ChildHWND). ChildHWND is created at a
		// fixed size at session-init and does NOT auto-track the parent resize
		// — we have to sync it explicitly here each frame so the runtime sees
		// the current window size, and so that both sides agree on tile dims.
		FDisplayXRViewConfig VC = Session->GetViewConfig();
		// VIEWS, not tiles. We begin on PRIMARY_STEREO, under which xrEndFrame
		// refuses a projection layer with viewCount > 2
		// (XR_ERROR_VALIDATION_FAILURE) — see DisplayXRMaxSubmittedViews. The
		// TILE geometry below (Cols/TW/TH) still comes from the mode, so on a
		// >2-tile mode we submit tiles 0 and 1 of that mode's grid and the
		// runtime's under-submit contract composites the frame.
		const int32 NV = VC.GetSubmittedViewCount();
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
		// (UE's stray top-level window is hidden + foreground handed back to the
		// shell earlier in the loop, before the shouldRender gate — see above.)
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

		// Texture-mode preview (#38): bind this layer to zone 1 — the same
		// zone the game thread chains on xrLocateViews. A frame becomes a
		// ZONES FRAME when a projection layer carries this chain; the zone
		// rect is the weave canvas, and without one the display processor
		// treats the canvas as the whole worst-case texture (magnified weave).
		XrDisplayZoneDXR Zone = {(XrStructureType)XR_TYPE_DISPLAY_ZONE_DXR};
		uint32 ZoneW = 0, ZoneH = 0;
		if (bSharedTextureMode && FDisplayXRPlatform::GetEditorZoneSize(ZoneW, ZoneH))
		{
			Zone.zoneId = 1;
			Zone.rect.offset = {0, 0};
			Zone.rect.extent = {(int32_t)ZoneW, (int32_t)ZoneH};
			Zone.next = PL.next;
			PL.next = &Zone;

			static bool bLoggedZoneSubmit = false;
			if (!bLoggedZoneSubmit)
			{
				bLoggedZoneSubmit = true;
				UE_LOG(LogDisplayXRCompositor, Log,
					TEXT("Compositor Thread: zones frame active — zone 1 = %ux%u chained on the projection layer"),
					ZoneW, ZoneH);
			}
		}

		const XrCompositionLayerBaseHeader* Layers[] = {(const XrCompositionLayerBaseHeader*)&PL};

		XrFrameEndInfo EI = {XR_TYPE_FRAME_END_INFO};
		EI.displayTime = FS.predictedDisplayTime;
		EI.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		EI.layerCount = 1;
		EI.layers = Layers;
		r = xrEndFrameFunc(XrSess, &EI);

		// A zones frame that reached the runtime = the display processor wove
		// into the shared texture. The presenter's publish gate counts these.
		if (XR_SUCCEEDED(r) && PL.next == &Zone)
		{
			ZonesFramesSubmitted.IncrementExchange();
		}

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

#if PLATFORM_WINDOWS
	// The Windows path is D3D12-only (D3D12 graphics binding, ID3D12Resource
	// swapchain images, a D3D12 RuntimeQueue). UE does not always run D3D12 when
	// the project asks for it: when the adapter cannot run any D3D12 shader format
	// the project cooked (e.g. an Intel UHD iGPU that tops out at SM5, against a
	// project that cooks only PCD3D_SM6 for D3D12), UE falls back to the D3D11 RHI
	// and RHIGetNativeDevice() returns an ID3D11Device*. Treating that as an
	// ID3D12Device dispatches the wrong vtable slot -- slot 8 is
	// ID3D11Device::CreateUnorderedAccessView, not CreateCommandQueue -- which is
	// the E_INVALIDARG "RuntimeQueue creation failed (0x80070057)" of #63. Refuse
	// cleanly instead of calling into an interface we do not have.
	if (!IsRHID3D12())
	{
		UE_LOG(LogDisplayXRCompositor, Warning,
			TEXT("Compositor: UE is running the %s RHI, not D3D12 -- no DisplayXR session (DisplayXR on Windows needs D3D12; if UE fell back because the adapter lacks SM6, cook PCD3D_SM5 for D3D12 too: +D3D12TargetedShaderFormats=PCD3D_SM5)"),
			GDynamicRHI ? GDynamicRHI->GetName() : TEXT("unknown"));
		GLog->Flush();
		return false;
	}
#endif
	if (!ResolveXrFunctions()) return false;

	// Create a dedicated runtime queue on UE's device.
	// DO NOT pass UE's main queue — the runtime may submit its own work which
	// would corrupt UE's RHI command-list tracking.
	if (!CreateRuntimeQueue()) return false;
	if (!CreateReleaseFence()) return false;

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
	//    editor-owned (the texture-mode proxy over the PIE viewport, or the
	//    legacy top-level mirror). Skip the child and bind the session
	//    directly.
	const bool bUseParentDirectly = (FDisplayXRPlatform::OverrideCompositorHWND != nullptr);
	ParentHWND = InParentHWND;  // track either way so Tick's rect call has an HWND
	if (!bUseParentDirectly)
	{
		if (!CreateChildWindow(InParentHWND)) return false;
	}
	void* const SessionHWND = bUseParentDirectly ? InParentHWND : ChildHWND;

	// Texture-mode preview (#38): create the shared surface the runtime weaves
	// into BEFORE session creation — its handle rides the window-binding chain.
	// On failure fall back to handle mode (the proxy window becomes a plain
	// bound window and the runtime presents into it — visible but harmless).
	bSharedTextureMode = bUseParentDirectly && FDisplayXRPlatform::bRequestSharedTextureBinding;
	if (bSharedTextureMode && !CreateWovenSharedTexture())
	{
		UE_LOG(LogDisplayXRCompositor, Warning,
			TEXT("Compositor: shared texture creation failed — falling back to handle-mode binding"));
		bSharedTextureMode = false;
	}

	// Create session with UE's device + our dedicated runtime queue + bound window.
	//
	// Editor PIE (override HWND set): the bound window — and the texture-vs-
	// handle mode — changes every play session while the XrSession persists on
	// the module, so a session surviving from a previous cycle is bound to a
	// window that no longer exists (and never saw this cycle's shared-texture
	// handle: the binding rides xrCreateSession). Rebind unconditionally —
	// CreateSessionWithGraphics destroys the old session first. Game mode keeps
	// the historical create-once behavior.
	const bool bForceRebind = bUseParentDirectly;
	if (!Session->IsSessionCreated() || bForceRebind) {
		if (!Session->CreateSessionWithGraphics(UEDevice, RuntimeQueue, SessionHWND,
			bSharedTextureMode ? WovenSharedHandle : nullptr)) {
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
	// Release fence liveness (#62): a frame's Signal may still be sitting in
	// UE's RHI queue. A runtime that GPU-waits the published fence
	// (displayxr-runtime#2362) would wait on it forever if it were never
	// submitted. Drain the render/RHI threads so every enqueued Signal reaches
	// the GPU before we stop the thread and destroy the swapchain.
	if (IsInGameThread() && GIsRHIInitialized)
	{
		FlushRenderingCommands();
	}
	StopCompositorThread();
	SwapchainImagesRHI.Empty();
	ArraySwapchainRHI.Empty();
	DestroySwapchain();
	DestroyWovenSharedTexture();
	DestroyChildWindow();
#if PLATFORM_WINDOWS
	if (RuntimeQueue) { static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release(); RuntimeQueue = nullptr; }
	if (ReleaseFence) { static_cast<ID3D12Fence*>(ReleaseFence)->Release(); ReleaseFence = nullptr; }
	ReleaseFenceValue = 0;
#endif
	if (BeginFrameReadyEvent) { FPlatformProcess::ReturnSynchEventToPool(BeginFrameReadyEvent); BeginFrameReadyEvent = nullptr; }
	if (EndFrameReadyEvent) { FPlatformProcess::ReturnSynchEventToPool(EndFrameReadyEvent); EndFrameReadyEvent = nullptr; }
	bReady = false;
}

bool FDisplayXRCompositor::CreateWovenSharedTexture()
{
#if PLATFORM_WINDOWS
	if (!Session) return false;

	// Worst-case atlas across all rendering modes (ADR-010 / INV-4.2 / INV-5.2):
	// allocated once, never resized. A window resize changes the zone rect the
	// runtime weaves into this surface, never the surface itself. The surface
	// itself is PROCESS-lifetime (module-owned cache) — creating it costs
	// ~1.5 s and the worst-case size never changes, so play-session restarts
	// reuse it and only rebind the same handle.
	uint32 W = 0, H = 0;
	Session->GetWorstCaseAtlasSize(W, H);
	if (!W || !H) return false;

	if (!FDisplayXRCoreModule::GetOrCreateWovenSurface(W, H, WovenSharedHandle, WovenTextureRHI))
	{
		return false;
	}
	UE_LOG(LogDisplayXRCompositor, Log,
		TEXT("Compositor: woven surface %ux%u bound (handle=%p, module-cached)"),
		W, H, WovenSharedHandle);
	return true;
#else
	return false;
#endif
}

void FDisplayXRCompositor::DestroyWovenSharedTexture()
{
	// References only — the surface itself is module-owned and outlives the
	// compositor (reused by the next play session's rebind).
	WovenTextureRHI.SafeRelease();
	WovenSharedHandle = nullptr;
	WovenResource = nullptr;
	bSharedTextureMode = false;
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

	// NOTE: the per-tick output-rect push is gone — xrSetSharedTextureOutputRect
	// was removed from the runtime API (ADR-031); display zones
	// (XR_DXR_display_zones) are the sole region paradigm.
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
// Release fence (#62)
// =============================================================================
bool FDisplayXRCompositor::CreateReleaseFence()
{
#if PLATFORM_WINDOWS
	ID3D12Device* UEDev = static_cast<ID3D12Device*>(UEDevice);
	ID3D12CommandQueue* RQ = static_cast<ID3D12CommandQueue*>(RuntimeQueue);
	if (!UEDev || !RQ) return false;

	// SHARED so a runtime on another device (the D3D11 hub, ADR-059) can
	// CreateSharedHandle + OpenSharedFence it and wait GPU-side.
	ID3D12Fence* Fence = nullptr;
	HRESULT hr = UEDev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&Fence));
	if (FAILED(hr) || !Fence)
	{
		UE_LOG(LogDisplayXRCompositor, Error, TEXT("Compositor: release fence creation failed (0x%08x)"), (unsigned)hr);
		return false;
	}
	ReleaseFence = Fence;
	ReleaseFenceValue = 0;

	const uint64 Zero = 0;
	RQ->SetPrivateDataInterface(DXR_UE_RELEASE_FENCE_GUID, Fence);
	RQ->SetPrivateData(DXR_UE_RELEASE_FENCE_VALUE_GUID, sizeof(Zero), &Zero);

	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: release fence %p (shared) published on RuntimeQueue"), ReleaseFence);
	return true;
#else
	return false;
#endif
}

void FDisplayXRCompositor::SignalReleaseFence_RenderThread(FRHICommandListImmediate& RHICmdList)
{
#if PLATFORM_WINDOWS
	ID3D12Fence* Fence = static_cast<ID3D12Fence*>(ReleaseFence);
	ID3D12CommandQueue* RQ = static_cast<ID3D12CommandQueue*>(RuntimeQueue);
	if (!Fence || !RQ) return;

	// Diagnostic kill switch (DXR_UE_RELEASE_FENCE=0): stop signalling and
	// publishing; the published value then stays where it was. Not a setting.
	static const bool bDisabled = FPlatformMisc::GetEnvironmentVariable(TEXT("DXR_UE_RELEASE_FENCE")).Equals(TEXT("0"));
	if (bDisabled)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogDisplayXRCompositor, Warning, TEXT("Compositor: release fence publishing DISABLED by DXR_UE_RELEASE_FENCE=0 (diagnostic)"));
		}
		RHICmdList.SubmitCommandsHint();
		return;
	}

	const uint64 Value = ++ReleaseFenceValue;

	// Why a Signal on UE's queue is allowed when foreign work is not: the rule
	// (AtlasHandoff.md) exists because raw ExecuteCommandLists on UE's queue
	// corrupts the RHI's own fence values / descriptor-heap / open-list
	// tracking. This is not that. RHISignalManualFence is the D3D12 RHI's own
	// API: on the RHI thread it closes the context's open command list and adds
	// (Fence, Value) to the payload's signal phase, so the RHI's submitter
	// issues the Signal itself, after every command list recorded before this
	// point (the scene render, the UI blend, the IPC slice copy and the
	// Present transition) on the queue that executed them. No commands are
	// recorded and none of UE's own fences move. It is the one write a reader
	// on another queue or device needs to order itself after UE's frame.
	//
	// EnqueueLambda because FD3D12CommandContext is only valid on the executing
	// (bottom-of-pipe) list. The fence is AddRef'd across the hop so a teardown
	// racing the RHI thread cannot free it under the lambda.
	Fence->AddRef();
	RHICmdList.EnqueueLambda(TEXT("DisplayXRReleaseFence"),
		[Fence, Value](FRHICommandListImmediate& ExecutingCmdList)
		{
			GetID3D12DynamicRHI()->RHISignalManualFence(ExecutingCmdList, Fence, Value);
			Fence->Release();
		});
	RHICmdList.SubmitCommandsHint();

	// No RuntimeQueue->Wait here, on purpose. A plug-in-side wait feeds the
	// in-process compositor's CPU idle-wait in xrEndFrame (layer_commit), so
	// xrEndFrame(N) would return only after UE's GPU frame N: it measured
	// ~25 % fewer presents and coincided with a DEVICE_HUNG on the split's
	// output device (#62). The runtime waits where it consumes the image
	// instead (displayxr-runtime#2362).
	//
	// Publish the value (contract above). Set before xrReleaseSwapchainImage,
	// on the same thread, so the runtime reads it inside that call.
	RQ->SetPrivateData(DXR_UE_RELEASE_FENCE_VALUE_GUID, sizeof(Value), &Value);

	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: release fence publishing (first value %llu; UE queue signals, no plug-in wait)"),
			(unsigned long long)Value);
	}
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
		// The D3D11 service creates IPC colour images fully TYPED (not
		// TYPELESS; MUTABLE_FORMAT is ignored there), so an _SRGB image (29)
		// must be wrapped with TexCreate_SRGB: a UNORM view of an _SRGB-typed
		// resource is invalid. The array is only ever a CopyTexture
		// destination, and a copy moves bytes without conversion, so the
		// encoded bytes still land unchanged.
		const ETextureCreateFlags ArrayFlags =
			ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource |
			(SwapchainFormat == 29 ? ETextureCreateFlags::SRGB : ETextureCreateFlags::None);
		ENQUEUE_RENDER_COMMAND(WrapDisplayXRArraySwapchain)(
			[&OutPrivate, &OutArray, InImages, DynamicRHI, W, H, ArrayFlags](FRHICommandListImmediate& RHICmdList)
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
						PF_R8G8B8A8, ArrayFlags, FClearValueBinding::Transparent, Res));
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
			// VIEWS, not tiles — i is the DESTINATION SLICE index below, and the
			// array swapchain is created DisplayXRMaxSubmittedViews slices wide
			// (CreateSwapchain: arraySize = ArrSize). Driving this loop off the
			// mode's tile count overran the array on any mode with more than
			// two tiles (4-tile mode -> DestSliceIndex 2 and 3 on a 2-slice
			// texture). The tile geometry still sources the copy (Cols/TW/TH),
			// so we lift tiles 0 and 1 out of UE's atlas. GetSubmittedViewCount()
			// is clamped to DisplayXRMaxSubmittedViews == ArrSize, so
			// DestSliceIndex can no longer run off the end.
			const int32 NV = VC.GetSubmittedViewCount();
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
			// The copy is recorded on UE's own list, so it executes on UE's main
			// queue after the render that produced its source. The release
			// fence is signalled after the copy, so a reader that waits on it
			// is ordered after the copy too (no CPU stall here:
			// BlockUntilGPUIdle halved the framerate).
			SignalReleaseFence_RenderThread(RHICmdList);
		}
	}
	else if (SwapchainTexture)
	{
		// In-process: UE rendered directly into the swapchain image (zero-copy),
		// or blitted the per-eye-UI atlas into it. Either way the last write is
		// on UE's main queue; the release fence is signalled after it.
		RHICmdList.Transition(FRHITransitionInfo(SwapchainTexture, ERHIAccess::Unknown, ERHIAccess::Present));
		SignalReleaseFence_RenderThread(RHICmdList);
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

	// Swapchain is worst-case sized across all rendering modes (INV-4.2) and
	// never reallocated — tiles are sub-images within it. By the time the
	// swapchain is created the session exists, so the enumerated mode table is
	// available; for every current mode set the max equals full panel pixels.
	Session->GetWorstCaseAtlasSize(SwapchainWidth, SwapchainHeight);
	UE_LOG(LogDisplayXRCompositor, Log, TEXT("Compositor: worst-case atlas → swapchain %dx%d"),
		SwapchainWidth, SwapchainHeight);
	GLog->Flush();
	if (!SwapchainWidth || !SwapchainHeight) return false;

	uint32_t FC = 0;
	xrEnumerateSwapchainFormatsFunc(S, 0, &FC, nullptr);
	if (!FC) return false;
	TArray<int64_t> Fmts; Fmts.SetNum(FC);
	xrEnumerateSwapchainFormatsFunc(S, FC, &FC, Fmts.GetData());

	// Colour: request the _SRGB format (runtime colour contract, ADR-044). UE's
	// post-processor writes ENCODED (display-referred) bytes, and a
	// format-honest runtime (>= v2.21.1 on D3D12) reads a UNORM swapchain as
	// LINEAR and sRGB-encodes it again on the way to the panel: double encode,
	// washed out. An _SRGB swapchain declares the bytes encoded. The runtime
	// creates the image TYPELESS (#1503) and we keep wrapping it with the
	// non-sRGB PF (no TexCreate_SRGB), so UE's views are plain UNORM and its
	// encoded bytes land unconverted -- the same arrangement UE's own OpenXRHMD
	// uses (sRGB swapchain + XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT). In-process
	// this relies on the runtime allocating TYPELESS images (#1503, runtime
	// >= v2.17.0, which this plug-in already requires). The IPC images are
	// TYPED by the service, so that array is wrapped _SRGB instead (see
	// WrapSwapchainImagesAsRHI); the slice copy stays a raw CopyTexture within
	// the R8G8B8A8 family either way.
	//
	// In-process single-tiled: BGRA, wrapped PF_B8G8R8A8 -> B8G8R8A8_UNORM_SRGB
	// (91). IPC array path: RGBA (proven coherent cross-process), wrapped
	// PF_R8G8B8A8 -> R8G8B8A8_UNORM_SRGB (29). The UNORM siblings (87 / 28) are
	// a fallback for a runtime that does not enumerate the _SRGB format.
	const int64 WantSRGB = bUseCopyPath ? 29 : 91;
	const int64 WantUNORM = bUseCopyPath ? 28 : 87;
	SwapchainFormat = 0;
	for (auto F : Fmts) { if (F == WantSRGB) { SwapchainFormat = F; break; } }
	if (!SwapchainFormat) {
		for (auto F : Fmts) { if (F == WantUNORM) { SwapchainFormat = F; break; } }
		if (!SwapchainFormat && !bUseCopyPath) {
			// Historical last resort of the in-process path (format list order).
			SwapchainFormat = Fmts[0];
			for (auto F : Fmts) { if (F == 28) { SwapchainFormat = F; break; } }
		}
		if (!SwapchainFormat) SwapchainFormat = WantUNORM;
		UE_LOG(LogDisplayXRCompositor, Warning,
			TEXT("Compositor: runtime does not offer DXGI format %lld (_SRGB); using %lld -- a format-honest runtime will show this washed out (ADR-044)"),
			(long long)WantSRGB, (long long)SwapchainFormat);
	}
	const bool bSRGBSwapchain = (SwapchainFormat == 29 || SwapchainFormat == 91);

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

	// The array swapchain is created ONCE per session but the rendering mode can
	// change under it (2D <-> 3D), so it is sized for the ceiling on views we
	// will ever submit rather than the current mode's count. The
	// ReleaseImage_RT copy loop is bounded by the same constant, which is what
	// keeps DestSliceIndex inside the array.
	const uint32 ArrSize = bUseCopyPath ? (uint32)DisplayXRMaxSubmittedViews : 1u;
	XrSwapchainCreateInfo CI = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	CI.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	// UE renders through UNORM views of the _SRGB image (see the colour note
	// above), which OpenXR requires the app to declare.
	if (bSRGBSwapchain) CI.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
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
