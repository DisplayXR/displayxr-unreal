// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRDevice.h"
#include "DisplayXRStereoMath.h"
#include "DisplayXRRigManager.h"
#include "DisplayXRPlatform.h"
#include "Widgets/SViewport.h"
#include "UnrealEngine.h"
#include "DynamicRHI.h"
#include "SceneView.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "RenderingThread.h"   // FlushRenderingCommands
#include "RenderGraphUtils.h"  // RegisterExternalTexture (editor atlas-copy path)
#include "XRCopyTexture.h"     // AddXRCopyTexturePass (format-safe blit, XRBase)

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Shared displayxr::math (displayxr-common submodule, Source/ThirdParty).
// The headers carry their own extern "C" guards.

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRDevice, Log, All);

// =============================================================================
// PIE-instrumentation helpers (Phase 1 of EditorPreviewNative.md investigation)
// Temporary: every log line below tagged [GAME]/[EDITOR]/[PIE] tells us which
// FDisplayXRDevice callbacks UE actually drives in editor PIE vs standalone.
// =============================================================================

static FORCEINLINE const TCHAR* WorldCtxTag()
{
#if WITH_EDITOR
	if (GIsPlayInEditorWorld) return TEXT("PIE");
	if (GIsEditor) return TEXT("EDITOR");
#endif
	return TEXT("GAME");
}

static FORCEINLINE const TCHAR* WorldTypeStr(EWorldType::Type T)
{
	switch (T)
	{
	case EWorldType::Game:          return TEXT("Game");
	case EWorldType::Editor:        return TEXT("Editor");
	case EWorldType::PIE:           return TEXT("PIE");
	case EWorldType::EditorPreview: return TEXT("EditorPreview");
	case EWorldType::GamePreview:   return TEXT("GamePreview");
	case EWorldType::GameRPC:       return TEXT("GameRPC");
	case EWorldType::Inactive:      return TEXT("Inactive");
	default:                        return TEXT("Unknown");
	}
}

// =============================================================================
// Windows WndProc subclass — drive live redraws during modal move/resize
// =============================================================================
//
// On Windows, a caption-bar drag or border-resize drag enters a modal loop in
// DefWindowProc (SC_MOVE / SC_SIZE) that blocks UE's main message pump. Without
// intervention the game thread is frozen, so ComputeViews never sees
// intermediate window rects — the off-axis frustum only catches up on mouse
// release, and resize-sizing of the atlas tiles only updates on mouse release.
//
// The reference test app cube_handle_d3d11_win (main.cpp:116-135) keeps
// rendering by leaving the window invalidated (no BeginPaint/EndPaint) and
// running RenderOneFrame synchronously from WM_PAINT inside the modal loop.
//
// Our port does the same, with a critical difference: UE's rendering is
// asynchronous (game thread queues draw commands for the render thread, which
// drives the compositor's swapchain Acquire/Release). Calling RedrawViewports
// alone is not enough — a second WM_PAINT can fire and Acquire the next
// swapchain image before the previous frame's Release has run, which corrupts
// the compositor's per-frame pairing. Synchronous completion via
// FlushRenderingCommands before returning keeps each paint an atomic frame.

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"

static WNDPROC  GDisplayXROrigWndProc = nullptr;
static HWND     GDisplayXRHookedHwnd = nullptr;
static bool     GDisplayXRInSizeMove = false;

static LRESULT CALLBACK DisplayXRSubclassedWndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
	switch (Msg)
	{
	case WM_ENTERSIZEMOVE:
		GDisplayXRInSizeMove = true;
		::InvalidateRect(Hwnd, nullptr, FALSE);
		break;

	case WM_EXITSIZEMOVE:
		GDisplayXRInSizeMove = false;
		break;

	case WM_SIZING:
	{
		// Bypass aspect-ratio enforcement. UE's FWindowsApplication (or the
		// DisplayXR/SR SDK runtime DLLs loaded into this process) handles
		// WM_SIZING by constraining the drag rect to a fixed aspect — even
		// with bShouldWindowPreserveAspectRatio=False in the project INI
		// (observed empirically: clean main still aspect-locks).
		//
		// Strategy: snapshot the user's actual drag rect, forward to the
		// original chain so UE keeps its internal state in sync, then overwrite
		// whatever aspect adjustment was applied. Return TRUE so Windows does
		// not also run DefWindowProc's handler.
		RECT* const pRect = reinterpret_cast<RECT*>(LParam);
		if (pRect)
		{
			const RECT UserRect = *pRect;
			if (GDisplayXROrigWndProc)
			{
				::CallWindowProcW(GDisplayXROrigWndProc, Hwnd, Msg, WParam, LParam);
			}
			*pRect = UserRect;
			return TRUE;
		}
		break;
	}

	case WM_PAINT:
		if (GDisplayXRInSizeMove && GEngine)
		{
			// Re-enter UE from inside the modal message loop. RedrawViewports
			// runs a viewport draw synchronously on this (main) thread, which
			// invokes SetupViewFamily on our FSceneViewExtension — ComputeViews
			// then sees the new HWND rect and rebuilds the projection.
			GEngine->RedrawViewports(false);
			// Wait for the render thread to fully consume the draw — this is
			// what makes each WM_PAINT atomic vs. the compositor's Acquire/
			// Release pairing. Without this, a second WM_PAINT acquires the
			// next swapchain image before the previous frame's Release has run,
			// leading to orphaned acquires and "ReleaseImage_RT: no image
			// acquired" errors after the drag ends.
			FlushRenderingCommands();
			// Stay invalidated so the modal loop keeps feeding us WM_PAINTs.
			::InvalidateRect(Hwnd, nullptr, FALSE);
			return 0; // deliberately skip UE's WM_PAINT (no BeginPaint → stays invalid)
		}
		break;
	}

	return GDisplayXROrigWndProc
		? ::CallWindowProcW(GDisplayXROrigWndProc, Hwnd, Msg, WParam, LParam)
		: ::DefWindowProcW(Hwnd, Msg, WParam, LParam);
}

static void DisplayXRInstallWndProcHook(HWND Hwnd)
{
	if (!Hwnd || GDisplayXRHookedHwnd == Hwnd) return;
	if (GDisplayXRHookedHwnd)
	{
		::SetWindowLongPtrW(GDisplayXRHookedHwnd, GWLP_WNDPROC, (LONG_PTR)GDisplayXROrigWndProc);
		GDisplayXROrigWndProc = nullptr;
		GDisplayXRHookedHwnd = nullptr;
	}
	GDisplayXROrigWndProc = (WNDPROC)::SetWindowLongPtrW(Hwnd, GWLP_WNDPROC, (LONG_PTR)DisplayXRSubclassedWndProc);
	GDisplayXRHookedHwnd = Hwnd;
}

static void DisplayXRRemoveWndProcHook()
{
	if (GDisplayXRHookedHwnd && GDisplayXROrigWndProc)
	{
		::SetWindowLongPtrW(GDisplayXRHookedHwnd, GWLP_WNDPROC, (LONG_PTR)GDisplayXROrigWndProc);
	}
	GDisplayXROrigWndProc = nullptr;
	GDisplayXRHookedHwnd = nullptr;
	GDisplayXRInSizeMove = false;
}

#include "Windows/HideWindowsPlatformTypes.h"
#endif // PLATFORM_WINDOWS


// =============================================================================
// Constructor
// =============================================================================

FDisplayXRDevice::FDisplayXRDevice(const FAutoRegister& AutoRegister, FDisplayXRSession* InSession)
	: FHeadMountedDisplayBase(nullptr)
	, FSceneViewExtensionBase(AutoRegister)
	, Session(InSession)
{
	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] DisplayXR Device: Created"), WorldCtxTag());
}

FDisplayXRDevice::~FDisplayXRDevice()
{
#if PLATFORM_WINDOWS
	DisplayXRRemoveWndProcHook();
#endif
	UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR Device: Destroyed"));
}

// =============================================================================
// IXRTrackingSystem
// =============================================================================

FName FDisplayXRDevice::GetSystemName() const
{
	return FName(TEXT("DisplayXR"));
}

int32 FDisplayXRDevice::GetXRSystemFlags() const
{
	return 0;
}

bool FDisplayXRDevice::EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type)
{
	if (Type == EXRTrackedDeviceType::Any || Type == EXRTrackedDeviceType::HeadMountedDisplay)
	{
		OutDevices.Add(IXRTrackingSystem::HMDDeviceId);
		return true;
	}
	return false;
}

bool FDisplayXRDevice::GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition)
{
	// Return false: we're a display device, not a head-mounted tracker.
	// Returning true with identity would lock the camera to face forward,
	// overriding mouse rotation.
	return false;
}

float FDisplayXRDevice::GetWorldToMetersScale() const
{
	return 100.0f;
}

void FDisplayXRDevice::ResetOrientationAndPosition(float Yaw)
{
}

void FDisplayXRDevice::OnBeginPlay(FWorldContext& InWorldContext)
{
	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] OnBeginPlay: ContextWorldType=%s pieInstance=%d"),
		WorldCtxTag(), WorldTypeStr(InWorldContext.WorldType), InWorldContext.PIEInstance);
	GLog->Flush();
	FHeadMountedDisplayBase::OnBeginPlay(InWorldContext);
}

// =============================================================================
// IHeadMountedDisplay
// =============================================================================

bool FDisplayXRDevice::IsHMDConnected()
{
	return Session && Session->IsActive();
}

bool FDisplayXRDevice::IsHMDEnabled() const
{
	return true;
}

void FDisplayXRDevice::EnableHMD(bool bEnable)
{
}

bool FDisplayXRDevice::GetHMDMonitorInfo(MonitorInfo& MonitorDesc)
{
	return true;
}

void FDisplayXRDevice::GetFieldOfView(float& InOutHFOVInDegrees, float& InOutVFOVInDegrees) const
{
}

void FDisplayXRDevice::SetInterpupillaryDistance(float NewInterpupillaryDistance)
{
}

float FDisplayXRDevice::GetInterpupillaryDistance() const
{
	return 6.3f;
}

bool FDisplayXRDevice::IsChromaAbCorrectionEnabled() const
{
	return false;
}

IHeadMountedDisplay* FDisplayXRDevice::GetHMDDevice()
{
	return this;
}

bool FDisplayXRDevice::GetHMDDistortionEnabled(EShadingPath ShadingPath) const
{
	return false;
}

// =============================================================================
// IStereoRendering
// =============================================================================

bool FDisplayXRDevice::IsStereoEnabled() const
{
	return true;
}

bool FDisplayXRDevice::EnableStereo(bool stereo)
{
	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] EnableStereo(%d) -> true"), WorldCtxTag(), stereo ? 1 : 0);
	GLog->Flush();
	return true;
}

void FDisplayXRDevice::AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const
{
	// Window-relative tile dims per the multiview-tiling spec (canvas = window
	// client area for handle apps): V_w = canvas_w * view_scale_x, V_h =
	// canvas_h * view_scale_y. Tiles live in the top-left of the panel-sized
	// swapchain image; the runtime's compositor reads tiles at the same
	// window-based offsets (it calls GetClientRect(app_hwnd) itself every
	// frame). CalculateRenderTargetSize stays panel-sized — changing UE's
	// logical RT size was the source of the prior eye-content bleed, not the
	// tile offsets themselves.
	// Editor in-tab texture mode (#38): UE renders into the viewport's OWN
	// render target (no separate swapchain RT — see ShouldUseSeparateRenderTarget),
	// so tiles derive from the INCOMING viewport dims. Those equal the proxy
	// window's client rect (the proxy is glued over this exact viewport), which
	// is what the compositor's ComputeTileDims measures for the submitted
	// imageRect — so the atlas copy in PostRenderViewFamily and the runtime's
	// sample rects stay aligned without depending on CacheWindowSize timing.
	if (FDisplayXRPlatform::bRequestSharedTextureBinding)
	{
		// Single source of truth for the tile math: the published zone size
		// (the proxy window's client rect, forced to EVEN dims at creation).
		// The compositor's ComputeTileDims measures the same rect via
		// GetClientRect, so with even dims RoundToInt(W*0.5) is exact on both
		// sides — no 1px divergence between UE's rendered tiles, the copy, and
		// the submitted imageRects (divergence = one eye samples the other's
		// edge). Falls back to the incoming viewport dims before the proxy
		// exists; always clamped into the incoming rect so views never exceed
		// the viewport's own render target.
		uint32 BaseW = SizeX, BaseH = SizeY;
		uint32 ZoneW = 0, ZoneH = 0;
		if (FDisplayXRPlatform::GetEditorZoneSize(ZoneW, ZoneH))
		{
			BaseW = FMath::Min(ZoneW, SizeX);
			BaseH = FMath::Min(ZoneH, SizeY);
		}
		const int32 Cols = FMath::Max(CachedViewConfig.TileColumns, 1);
		const int32 Rows = FMath::Max(CachedViewConfig.TileRows, 1);
		int32 TileW = FMath::Max(1, FMath::RoundToInt(BaseW * CachedViewConfig.ScaleX));
		int32 TileH = FMath::Max(1, FMath::RoundToInt(BaseH * CachedViewConfig.ScaleY));
		TileW = FMath::Min(TileW, (int32)SizeX / Cols);
		TileH = FMath::Min(TileH, (int32)SizeY / Rows);
		X = (ViewIndex % Cols) * TileW;
		Y = (ViewIndex / Cols) * TileH;
		SizeX = TileW;
		SizeY = TileH;
		return;
	}

	CacheWindowSize();
	// Window-relative tile dims (both paths). The IPC array copy + projection use
	// the SAME window-relative dims (compositor's ComputeTileDims, same HWND), so
	// the content aspect tracks the window → correct under resize (like the cube),
	// while the array slice stays fixed-size so it never trips needs_scale.
	const int32 TileW = FMath::Max(1, FMath::RoundToInt(CachedWindowW * CachedViewConfig.ScaleX));
	const int32 TileH = FMath::Max(1, FMath::RoundToInt(CachedWindowH * CachedViewConfig.ScaleY));

	const int32 Cols = FMath::Max(CachedViewConfig.TileColumns, 1);
	const int32 Col = ViewIndex % Cols;
	const int32 Row = ViewIndex / Cols;
	X = Col * TileW;
	Y = Row * TileH;
	SizeX = TileW;
	SizeY = TileH;
}

FMatrix FDisplayXRDevice::GetStereoProjectionMatrix(const int32 ViewIndex) const
{
	if (ViewIndex >= 0 && ViewIndex < CachedViews.Num())
	{
		return CachedViews[ViewIndex].ProjectionMatrix;
	}
	return CachedCenter.ProjectionMatrix;
}

void FDisplayXRDevice::CalculateStereoViewOffset(const int32 ViewIndex, FRotator& ViewRotation,
	const float WorldToMeters, FVector& ViewLocation)
{
	if (ViewIndex >= 0 && ViewIndex < CachedViews.Num())
	{
		ViewLocation += ViewRotation.Quaternion().RotateVector(CachedViews[ViewIndex].Offset);
	}
}

int32 FDisplayXRDevice::GetDesiredNumberOfViews(bool bStereoRequested) const
{
	if (!bStereoRequested)
	{
		return 1;
	}
	const int32 Count = CachedViewConfig.GetViewCount();
	// UE requires at least 2 views when stereo is enabled
	return FMath::Max(Count, 2);
}

TSharedPtr<IStereoRendering, ESPMode::ThreadSafe> FDisplayXRDevice::GetStereoRenderingDevice()
{
	return SharedThis(this);
}

IStereoRenderTargetManager* FDisplayXRDevice::GetRenderTargetManager()
{
	return this;
}

// =============================================================================
// FXRRenderTargetManager
// =============================================================================

bool FDisplayXRDevice::ShouldUseSeparateRenderTarget() const
{
	// Editor in-tab texture mode (#38): NO separate render target. Slate takes
	// the stereo-composite path for any window whose backing viewport has
	// UseSeparateRenderTarget() && IsStereoscopic3D() — it renders the ENTIRE
	// window's UI into GetViewportRenderTargetTexture() (our swapchain!) and
	// expects RenderTexture_RenderThread to composite it back. In "Selected
	// Viewport" PIE that window is the main editor frame, so the editor UI
	// stomped the atlas and the DP wove menu bars (proven via
	// DisplayXR.CaptureAtlas). With false, UE renders the SBS atlas into the
	// viewport's OWN target and PostRenderViewFamily copies it into the
	// swapchain — one extra copy, editor-only. Game path keeps zero-copy.
	const bool bSeparate = !FDisplayXRPlatform::bRequestSharedTextureBinding;

	return bSeparate;
}

bool FDisplayXRDevice::AllocateRenderTargetTexture(uint32 Index, uint32 SizeX, uint32 SizeY, uint8 Format,
	uint32 NumMips, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags,
	FTextureRHIRef& OutTargetableTexture, FTextureRHIRef& OutShaderResourceTexture, uint32 NumSamples)
{
	// Editor in-tab texture mode: refuse. FSceneViewport's allocation sites run
	// REGARDLESS of ShouldUseSeparateRenderTarget, and a successful allocation
	// here parks the viewport on a separate RT anyway — which starves Slate
	// presents (the window only repaints on input) and re-opens the
	// stereo-composite trap. Returning false keeps the viewport on its normal
	// window-backed target; PostRenderViewFamily copies the atlas out of it.
	if (FDisplayXRPlatform::bRequestSharedTextureBinding)
	{
		return false;
	}

	// Singular allocator. Only hit when AllocateRenderTargetTextures returns
	// false (compositor not yet ready). Force B8G8R8A8 so the transient default
	// RT matches swapchain format expectations.
	const EPixelFormat DesiredFormat = PF_B8G8R8A8;

	FRHITextureCreateDesc Desc =
		FRHITextureCreateDesc::Create2D(TEXT("DisplayXRAtlas"), SizeX, SizeY, DesiredFormat)
		.SetNumMips(NumMips)
		.SetNumSamples(NumSamples)
		.SetFlags(Flags | TargetableTextureFlags | ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);

	OutTargetableTexture = OutShaderResourceTexture = RHICreateTexture(Desc);

	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] AllocateRenderTargetTexture (singular fallback) %ux%u fmt=%d"),
		WorldCtxTag(), SizeX, SizeY, (int)DesiredFormat);
	GLog->Flush();

	return OutTargetableTexture.IsValid();
}

bool FDisplayXRDevice::AllocateRenderTargetTextures(uint32 SizeX, uint32 SizeY, uint8 Format,
	uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags,
	TArray<FTextureRHIRef>& OutTargetableTextures,
	TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples)
{
	// Editor in-tab texture mode: refuse (see AllocateRenderTargetTexture).
	if (FDisplayXRPlatform::bRequestSharedTextureBinding)
	{
		return false;
	}

	if (!Compositor.IsValid() || !Compositor->IsReady())
	{
		// Fall back to UE's default / singular allocator until compositor is ready.
		return false;
	}

	TArray<FTextureRHIRef> Wrapped;
	if (!Compositor->GetSwapchainImagesRHI(Wrapped) || Wrapped.Num() == 0)
	{
		return false;
	}

	OutTargetableTextures = Wrapped;
	OutShaderResourceTextures = Wrapped;

	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] AllocateRenderTargetTextures -> %d swapchain images (%ux%u)"),
		WorldCtxTag(), Wrapped.Num(), SizeX, SizeY);
	return true;
}

int32 FDisplayXRDevice::AcquireColorTexture()
{
	check(IsInGameThread());
	if (!Compositor.IsValid() || !Compositor->IsReady()) return -1;
	return Compositor->AcquireImage_GameThread();
}

EPixelFormat FDisplayXRDevice::GetActualColorSwapchainFormat() const
{
	return PF_B8G8R8A8;
}

void FDisplayXRDevice::CalculateRenderTargetSize(const FViewport& Viewport, uint32& InOutSizeX, uint32& InOutSizeY)
{
	// Editor in-tab texture mode: leave the size alone. The viewport renders
	// into its own window-sized target on the normal Slate paint path; any
	// stereo-driven resize here half-engages the separate-RT machinery, which
	// stalls Slate invalidation (the scene only renders when Slate paints) and
	// starves the whole preview.
	if (FDisplayXRPlatform::bRequestSharedTextureBinding)
	{
		return;
	}

	// When compositor is ready we render directly into swapchain images, which
	// are at full display resolution (e.g. 3840x2160). Tiles are sub-rects.
	if (Compositor.IsValid() && Compositor->IsReady())
	{
		const uint32 SwW = Compositor->GetSwapchainWidth();
		const uint32 SwH = Compositor->GetSwapchainHeight();
		if (SwW && SwH)
		{
			InOutSizeX = SwW;
			InOutSizeY = SwH;
		}
	}
	else
	{
		InOutSizeX = CachedViewConfig.GetAtlasW();
		InOutSizeY = CachedViewConfig.GetAtlasH();
		if (InOutSizeX == 0) InOutSizeX = 1920;
		if (InOutSizeY == 0) InOutSizeY = 1080;
	}

}

bool FDisplayXRDevice::NeedReAllocateViewportRenderTarget(const FViewport& Viewport)
{
	// Editor in-tab texture mode: no separate RT, so no reallocation ever.
	if (FDisplayXRPlatform::bRequestSharedTextureBinding)
	{
		return false;
	}

	// One-shot reallocation trigger when the compositor transitions to ready,
	// so UE re-runs AllocateRenderTargetTextures with the swapchain-backed array.
	if (Compositor.IsValid() && Compositor->IsReady() && bSwapchainRTReallocPending)
	{
		bSwapchainRTReallocPending = false;
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] NeedReAllocateViewportRenderTarget -> true (compositor became ready)"), WorldCtxTag());
		GLog->Flush();
		return true;
	}

	const FIntPoint RenderTargetSize = Viewport.GetRenderTargetTextureSizeXY();
	uint32 NewSizeX = 0, NewSizeY = 0;
	CalculateRenderTargetSize(Viewport, NewSizeX, NewSizeY);
	const bool bNeed = (NewSizeX != (uint32)RenderTargetSize.X || NewSizeY != (uint32)RenderTargetSize.Y);
	if (bNeed)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] NeedReAllocateViewportRenderTarget -> true (size %dx%d -> %ux%u)"),
			WorldCtxTag(), RenderTargetSize.X, RenderTargetSize.Y, NewSizeX, NewSizeY);
		GLog->Flush();
	}
	return bNeed;
}

void FDisplayXRDevice::CacheWindowSize() const
{
	// Default to DisplayXR panel dims so downstream math always has a valid pair.
	const FDisplayXRDisplayInfo DI = Session ? Session->GetDisplayInfo() : FDisplayXRDisplayInfo();
	uint32 W = DI.DisplayPixelWidth  > 0 ? (uint32)DI.DisplayPixelWidth  : 1920;
	uint32 H = DI.DisplayPixelHeight > 0 ? (uint32)DI.DisplayPixelHeight : 1080;

#if PLATFORM_WINDOWS
	// Measure the BOUND window (overlay) once the compositor exists — under the
	// shell the runtime sizes it to the workspace window, so UE renders at the
	// workspace size and content tracks a resize. Falls back to UE's own window
	// (GameHWND) before the compositor/overlay exists.
	HWND MeasureHWND = (HWND)GameHWND;
	if (Compositor.IsValid() && Compositor->GetBoundHWND())
	{
		MeasureHWND = (HWND)Compositor->GetBoundHWND();
	}
	if (MeasureHWND)
	{
		RECT rc = {};
		if (::GetClientRect(MeasureHWND, &rc))
		{
			const int32 RectW = rc.right  - rc.left;
			const int32 RectH = rc.bottom - rc.top;
			if (RectW > 0 && RectH > 0)
			{
				// Clamp to panel size: user dragging larger than the panel would
				// ask the compositor to crop a region bigger than the swapchain,
				// which isn't allocated for that size (spec: swapchain is worst-
				// case sized at init and never reallocated).
				W = FMath::Min<uint32>((uint32)RectW, W);
				H = FMath::Min<uint32>((uint32)RectH, H);
			}
		}
	}
#endif

	CachedWindowW = W;
	CachedWindowH = H;
}

static TSharedPtr<SWidget> GetTopMostWidget(TSharedPtr<SWidget> Widget)
{
	if (!Widget.IsValid())
	{
		return {};
	}
	TSharedPtr<SWidget> Parent = Widget->GetParentWidget();
	if (Parent.IsValid())
	{
		return GetTopMostWidget(Parent);
	}
	return Widget;
}

void FDisplayXRDevice::UpdateViewport(bool bUseSeparateRenderTarget, const FViewport& Viewport, SViewport* ViewportWidget)
{
	FXRRenderTargetManager::UpdateViewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);

	// Resolve the game window HWND from the viewport widget chain. Cached on the
	// device so ComputeViews can re-query window rect each frame for window-
	// relative Kooima math.
	void* WindowHandle = nullptr;
	if (ViewportWidget)
	{
		TSharedPtr<SWidget> WindowWidget = GetTopMostWidget(ViewportWidget->GetParentWidget());
		if (WindowWidget.IsValid())
		{
			SWindow* WindowPtr = static_cast<SWindow*>(WindowWidget.Get());
			if (WindowPtr->GetNativeWindow().IsValid())
			{
				WindowHandle = WindowPtr->GetNativeWindow()->GetOSWindowHandle();
			}
		}
	}
	GameHWND = WindowHandle;

#if PLATFORM_WINDOWS
	// Install a WndProc subclass on the game window so we can:
	//   - keep UE ticking during an interactive move/resize modal loop (WM_PAINT),
	//   - bypass aspect-ratio constraint applied by UE / runtime DLLs on WM_SIZING.
	if (GameHWND)
	{
		DisplayXRInstallWndProcHook((HWND)GameHWND);
	}
#endif

	// Deferred compositor creation: create the compositor (which owns session
	// creation with graphics binding) once we have D3D device and game window HWND.
	if (Session && !Compositor && !bCompositorCreationAttempted)
	{
		bCompositorCreationAttempted = true;

		// HWND selection: editor native-PIE sets FDisplayXRPlatform::
		// OverrideCompositorHWND to a raw-Win32 mirror so the runtime has a
		// dedicated window to target, independent of the PIE viewport (which
		// is embedded in the editor). Game mode leaves the override null and
		// uses the viewport-widget HWND already resolved above.
		if (FDisplayXRPlatform::OverrideCompositorHWND)
		{
			WindowHandle = FDisplayXRPlatform::OverrideCompositorHWND;
			UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] UpdateViewport: using OverrideCompositorHWND=%p"),
				WorldCtxTag(), WindowHandle);
		}


		// Get D3D device and command queue from UE's RHI
		void* D3DDevice = GDynamicRHI ? GDynamicRHI->RHIGetNativeDevice() : nullptr;
		void* CommandQueue = GDynamicRHI ? GDynamicRHI->RHIGetNativeGraphicsQueue() : nullptr;

		if (D3DDevice && WindowHandle)
		{
			Compositor = MakeUnique<FDisplayXRCompositor>(Session);
			if (!Compositor->Initialize(WindowHandle, D3DDevice, CommandQueue))
			{
				UE_LOG(LogDisplayXRDevice, Warning, TEXT("[%s] DisplayXR Device: Compositor initialization failed"), WorldCtxTag());
				Compositor.Reset();
			}
		}
	}

	// Tick compositor for deferred swapchain creation
	if (Compositor)
	{
		Compositor->Tick();
	}
}

void FDisplayXRDevice::ShutdownCompositorForSessionEnd()
{
	check(IsInGameThread());

	// Stay disarmed: creation only re-opens at RearmCompositorCreation(), so a
	// trailing UpdateViewport during teardown cannot bind a new compositor to a
	// window that is about to be destroyed.
	bCompositorCreationAttempted = true;

	if (!Compositor)
	{
		return;
	}

	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] ShutdownCompositorForSessionEnd: tearing down compositor"),
		WorldCtxTag());
	GLog->Flush();

	// UE's viewport render targets ARE the OpenXR swapchain images (zero-copy
	// handoff), so the render thread must be idle before the compositor
	// destroys the swapchain underneath them.
	FlushRenderingCommands();

	Compositor.Reset();

	// The XrSession outlives the play session, still bound to a window that is
	// about to be destroyed. Park view location until the next rebind so the
	// next Play doesn't render camera jumps off the zombie binding.
	if (Session)
	{
		Session->ParkForRebind();
	}

	// The wrapped swapchain textures we handed UE are gone; make the next
	// compositor's readiness re-trigger AllocateRenderTargetTextures.
	bSwapchainRTReallocPending = true;
	CachedWindowW = 0;
	CachedWindowH = 0;
}

void FDisplayXRDevice::RearmCompositorCreation()
{
	check(IsInGameThread());

	// Re-arm even if a compositor somehow survived: UpdateViewport only builds
	// one when the pointer is null, so this cannot double-create.
	bCompositorCreationAttempted = false;

	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] RearmCompositorCreation: deferred compositor creation re-armed"),
		WorldCtxTag());
	GLog->Flush();
}

void FDisplayXRDevice::RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer,
	FRDGTextureRef SrcTexture, FVector2f WindowSize) const
{
	// Zero-copy path: SrcTexture IS the OpenXR swapchain image UE rendered into.
	// The OpenXR compositor owns display output on the light-field panel.
	//
	// A host-window preview blit (center-view tile → game window backbuffer)
	// that respects window resize is tracked as TODO: previous attempts via
	// FPixelShaderUtils::AddFullscreenPass + FCopyRectPS failed D3D12 PSO
	// creation with E_INVALIDARG when the Slate backbuffer format was HDR10
	// (R10G10B10A2_UNORM). Needs a different RDG pattern — possibly using
	// AddDrawTexturePass for format conversion + a separate scale pass, or
	// a compute-shader copy. Left as a no-op for now; combined with Issues 3+5
	// the window still resizes freely and the content region tracks it, but
	// the game-window preview may show black-pad regions where the
	// content-region rect is smaller than the host window.
}

void FDisplayXRDevice::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	if (!Compositor.IsValid() || !Compositor->IsReady()) return;

	FDisplayXRCompositor* Comp = Compositor.Get();

	// The view family's render target: the swapchain image on the zero-copy
	// game path, the viewport's own RT in editor texture mode.
	FRHITexture* SrcTextureRHI = nullptr;
	if (InViewFamily.RenderTarget)
	{
		SrcTextureRHI = InViewFamily.RenderTarget->GetRenderTargetTexture();
	}

	// ------------------------------------------------------------------
	// Editor in-tab texture mode (#38): UE rendered the SBS atlas into the
	// viewport's OWN target (Slate must never see a stereo separate-RT
	// window — it would composite the whole editor UI into the swapchain).
	// Hand the atlas to the runtime here instead: acquire a swapchain image,
	// blit the tile region, release. One extra copy per frame, editor-only.
	// ------------------------------------------------------------------
	if (FDisplayXRPlatform::bRequestSharedTextureBinding)
	{
		if (!SrcTextureRHI) return;

		// Events + xr calls + atomics only — thread-agnostic despite the name;
		// the engine no longer acquires for us with the separate RT off.
		const int32 Idx = Comp->AcquireImage_GameThread();
		if (Idx < 0) return;
		FTextureRHIRef DstRef = Comp->GetSwapchainImageRHI(Idx);
		if (!DstRef.IsValid())
		{
			return;
		}

		// The union of the family's view rects is exactly the tile region UE
		// wrote (AdjustViewRect math). Same rect on both sides: tiles sit at
		// identical offsets in the viewport RT and the swapchain, matching the
		// imageRects the compositor submits from the proxy's client size.
		FIntRect AtlasRect(0, 0, 0, 0);
		for (const FSceneView* View : InViewFamily.Views)
		{
			if (View)
			{
				AtlasRect.Union(View->UnscaledViewRect);
			}
		}
		AtlasRect.Clip(FIntRect(0, 0, SrcTextureRHI->GetSizeX(), SrcTextureRHI->GetSizeY()));
		AtlasRect.Clip(FIntRect(0, 0, DstRef->GetSizeX(), DstRef->GetSizeY()));
		if (AtlasRect.IsEmpty()) return;

		FRDGTextureRef SrcRDG = GraphBuilder.FindExternalTexture(SrcTextureRHI);
		if (!SrcRDG)
		{
			SrcRDG = RegisterExternalTexture(GraphBuilder, SrcTextureRHI, TEXT("DisplayXRAtlasSrc"));
		}
		FRDGTextureRef DstRDG = RegisterExternalTexture(GraphBuilder, DstRef.GetReference(), TEXT("DisplayXRSwapchainDst"));

		FXRCopyTextureOptions Options(GMaxRHIFeatureLevel);
		Options.LoadAction = ERenderTargetLoadAction::ELoad;
		Options.BlendMod = EXRCopyTextureBlendModifier::Opaque;
		AddXRCopyTexturePass(GraphBuilder, RDG_EVENT_NAME("DisplayXRAtlasToSwapchain"),
			SrcRDG, AtlasRect, DstRDG, AtlasRect, Options);

		static int32 CopyCount = 0;
		++CopyCount;
		if (CopyCount == 1 || CopyCount % 300 == 0)
		{
			UE_LOG(LogDisplayXRDevice, Log,
				TEXT("[%s] Atlas copy %s (#%d): rect=(%d,%d %dx%d) src=%ux%u -> swapchain[%d] %ux%u"),
				WorldCtxTag(), CopyCount == 1 ? TEXT("ACTIVE") : TEXT("alive"), CopyCount,
				AtlasRect.Min.X, AtlasRect.Min.Y, AtlasRect.Width(), AtlasRect.Height(),
				SrcTextureRHI->GetSizeX(), SrcTextureRHI->GetSizeY(),
				Idx, DstRef->GetSizeX(), DstRef->GetSizeY());
		}

		FRHITexture* DstRHI = DstRef.GetReference();
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("DisplayXR_ReleaseSwapchain"),
			ERDGPassFlags::NeverCull,
			[Comp, DstRHI](FRHICommandListImmediate& RHICmdList)
			{
				Comp->ReleaseImage_RenderThread(RHICmdList, DstRHI);
			});
		return;
	}

	// Atlas capture is now runtime-owned (xrCaptureAtlasDXR, driven from
	// FDisplayXRAtlasCapture::RequestCapture); no app-side RHI readback here.
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("DisplayXR_ReleaseSwapchain"),
		ERDGPassFlags::NeverCull,
		[Comp, SrcTextureRHI](FRHICommandListImmediate& RHICmdList)
		{
			Comp->ReleaseImage_RenderThread(RHICmdList, SrcTextureRHI);
		});
}

// =============================================================================
// FSceneViewExtensionBase
// =============================================================================

void FDisplayXRDevice::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	if (!Session)
	{
		return;
	}

	Session->Tick();

	// Tick compositor for deferred swapchain creation (needs session running)
	if (Compositor)
	{
		Compositor->Tick();
	}

	CachedViewConfig = Session->GetViewConfig();
	ComputeViews();
}

void FDisplayXRDevice::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
}

void FDisplayXRDevice::SetupViewPoint(APlayerController* Player, FMinimalViewInfo& InViewInfo)
{
	PlayerViewLocation_GameThread = InViewInfo.Location;

	// Don't modify InViewInfo — let UE handle camera rotation naturally.
	// The per-view offset is applied in CalculateStereoViewOffset.
}

void FDisplayXRDevice::SetupViewProjectionMatrix(FSceneViewProjectionData& InOutProjectionData)
{
	// Don't override — when stereo is enabled, UE uses GetStereoProjectionMatrix
	// instead of this callback (see LocalPlayer.cpp:1269).
}

void FDisplayXRDevice::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FDisplayXRDevice::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
	// Game-thread CalculateStereoViewOffset + GetStereoProjectionMatrix handle
	// the per-view setup. No render-thread override needed — doing so would
	// fight UE's view matrix which already includes the mouse rotation.
}

int32 FDisplayXRDevice::GetPriority() const
{
	return INT32_MIN + 10;
}

bool FDisplayXRDevice::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	const bool bResult = GEngine && GEngine->IsStereoscopic3D(Context.Viewport);

	// Log first call AND every transition: this is the gate that decides whether
	// any FSceneViewExtensionBase callback (SetupViewFamily etc.) fires at all.
	// In editor PIE this is expected to be false unless the SViewport opts in via
	// EnableStereoRendering(true) — which only happens for VR Preview by default.
	static bool bLoggedFirst = false;
	static bool bLastResult = false;
	if (!bLoggedFirst || bLastResult != bResult)
	{
		const FViewport* V = Context.Viewport;
		const bool bAllow = V ? V->IsStereoRenderingAllowed() : true;
		const bool bDevValid = GEngine && GEngine->StereoRenderingDevice.IsValid();
		const bool bDevEnabled = bDevValid && GEngine->StereoRenderingDevice->IsStereoEnabled();
		UE_LOG(LogDisplayXRDevice, Log,
			TEXT("[%s] IsActiveThisFrame_Internal %s -> %d (viewport=%p IsStereoRenderingAllowed=%d StereoDeviceValid=%d StereoDeviceEnabled=%d)"),
			WorldCtxTag(),
			bLoggedFirst ? TEXT("change") : TEXT("first-call"),
			bResult ? 1 : 0, V, bAllow ? 1 : 0, bDevValid ? 1 : 0, bDevEnabled ? 1 : 0);
		GLog->Flush();
		bLoggedFirst = true;
		bLastResult = bResult;
	}
	return bResult;
}

// =============================================================================
// View computation (Kooima integration)
// =============================================================================

void FDisplayXRDevice::ComputeViews()
{
	const FDisplayXRDisplayInfo DI = Session->GetDisplayInfo();
	const FDisplayXRTunables T = Session->GetTunables();
	const int32 ViewCount = FMath::Max(CachedViewConfig.GetViewCount(), 2);

	// Read raw eye positions from session (OpenXR display-local, meters)
	FVector LeftEyeRaw, RightEyeRaw;
	bool bTracked = false;
	Session->GetEyePositions(LeftEyeRaw, RightEyeRaw, bTracked);

	// If no eye data yet (session not running), use nominal viewer position
	// with a standard IPD offset so we get a valid default view
	bool bUsedFallback = false;
	if (LeftEyeRaw.IsNearlyZero() && RightEyeRaw.IsNearlyZero())
	{
		bUsedFallback = true;
		const float DefaultIPD = 0.063f; // 63mm in meters
		const float NomX = (float)DI.NominalViewerPosition.X;
		const float NomY = (float)DI.NominalViewerPosition.Y;
		const float NomZ = DI.bIsValid ? (float)DI.NominalViewerPosition.Z : 0.5f;
		LeftEyeRaw = FVector(NomX - DefaultIPD * 0.5f, NomY, NomZ);
		RightEyeRaw = FVector(NomX + DefaultIPD * 0.5f, NomY, NomZ);
	}

	// Diagnostic: log eye input and fallback state
	static int32 CVCount = 0;
	CVCount++;
	if (CVCount <= 3 || CVCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRDevice, Log,
			TEXT("ComputeViews #%d: tracked=%d fallback=%d L=(%f,%f,%f) R=(%f,%f,%f)"),
			CVCount, bTracked, bUsedFallback,
			LeftEyeRaw.X, LeftEyeRaw.Y, LeftEyeRaw.Z,
			RightEyeRaw.X, RightEyeRaw.Y, RightEyeRaw.Z);
		GLog->Flush();
	}

	// -----------------------------------------------------------------------
	// XR_DXR_view_rig (#396 W7, ADR-024): the runtime already applied the rig
	// during xrLocateViews, so each view arrives render-ready — pose.position is
	// the eye displacement in the rig's space and fov is the off-axis frustum.
	// There is no app-side view math left: no window-rect resolve (the runtime
	// owns the canvas), no eye factoring, no convergence-plane frustum.
	//
	// Without the extension the plugin has no view math of its own, so there is
	// nothing to fall back to — warn once and leave the cached views identity so
	// the frame renders mono rather than wrong.
	// -----------------------------------------------------------------------
	CachedViews.SetNum(ViewCount);

	// Union of the per-view fovs, for the center/mono view built at the end.
	XrFovf CenterFov = {};
	bool bCenterFovValid = false;

	if (!Session->HasViewRig())
	{
		static bool bWarnedNoViewRig = false;
		if (!bWarnedNoViewRig)
		{
			bWarnedNoViewRig = true;
			UE_LOG(LogDisplayXRDevice, Warning,
				TEXT("Runtime does not advertise %s — the plugin no longer computes the "
				     "view math itself (#396 W7). Stereo is disabled; update to a "
				     "DisplayXR runtime >= v2.0.0."),
				TEXT(XR_DXR_VIEW_RIG_EXTENSION_NAME));
			GLog->Flush();
		}
		for (int32 i = 0; i < ViewCount; i++)
		{
			CachedViews[i].Offset = FVector::ZeroVector;
			CachedViews[i].ProjectionMatrix = FMatrix::Identity;
		}
	}
	else
	{
		for (int32 i = 0; i < ViewCount; i++)
		{
			// Views beyond what the runtime located clamp to the last one, the
			// same way the Kooima path clamped to its 2-eye output.
			FVector ViewPos;
			FQuat ViewOrient;
			XrFovf ViewFov = {};
			int32 SrcIdx = i;
			while (SrcIdx > 0 && !Session->GetViewData(SrcIdx, ViewPos, ViewOrient, ViewFov))
			{
				SrcIdx--;
			}
			if (!Session->GetViewData(SrcIdx, ViewPos, ViewOrient, ViewFov))
			{
				CachedViews[i].Offset = FVector::ZeroVector;
				CachedViews[i].ProjectionMatrix = FMatrix::Identity;
				continue;
			}

			// The rig pose we submitted is the origin, so the located position
			// IS the camera-local (unrotated) eye displacement UE wants —
			// CalculateStereoViewOffset rotates it by the view rotation.
			const XrVector3f EyeLocal = {
				(float)ViewPos.X, (float)ViewPos.Y, (float)ViewPos.Z
			};
			CachedViews[i].Offset = OpenXRPositionToUE(EyeLocal);
			CachedViews[i].ProjectionMatrix = ProjectionMatrixFromFov(ViewFov);

			if (!bCenterFovValid)
			{
				CenterFov = ViewFov;
				bCenterFovValid = true;
			}
			else
			{
				CenterFov.angleLeft  = FMath::Min(CenterFov.angleLeft,  ViewFov.angleLeft);
				CenterFov.angleRight = FMath::Max(CenterFov.angleRight, ViewFov.angleRight);
				CenterFov.angleDown  = FMath::Min(CenterFov.angleDown,  ViewFov.angleDown);
				CenterFov.angleUp    = FMath::Max(CenterFov.angleUp,    ViewFov.angleUp);
			}
		}
	}

	// Diagnostic: log computed offsets
	if (CVCount <= 3 || CVCount % 300 == 0)
	{
		if (CachedViews.Num() >= 2)
		{
			UE_LOG(LogDisplayXRDevice, Log,
				TEXT("ComputeViews #%d: offset[0]=(%f,%f,%f) offset[1]=(%f,%f,%f) cameraCentric=%d"),
				CVCount,
				CachedViews[0].Offset.X, CachedViews[0].Offset.Y, CachedViews[0].Offset.Z,
				CachedViews[1].Offset.X, CachedViews[1].Offset.Y, CachedViews[1].Offset.Z,
				T.bCameraCentric);
			GLog->Flush();
		}
	}

	// Center view: average of all views
	CachedCenter.Offset = FVector::ZeroVector;
	for (int32 i = 0; i < ViewCount; i++)
	{
		CachedCenter.Offset += CachedViews[i].Offset;
	}
	CachedCenter.Offset /= (float)ViewCount;

	// Center projection: the union of the per-view frustums, so the center view
	// covers everything any eye can see. Built from the same runtime-supplied
	// fovs as the per-view matrices — there is no app-side frustum math left to
	// re-derive it from (#396 W7).
	CachedCenter.ProjectionMatrix = bCenterFovValid
		? ProjectionMatrixFromFov(CenterFov)
		: FMatrix::Identity;
}
