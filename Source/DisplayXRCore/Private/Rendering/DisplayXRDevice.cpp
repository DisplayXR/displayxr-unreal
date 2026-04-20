// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRDevice.h"
#include "DisplayXRStereoMath.h"
#include "DisplayXRRigManager.h"
#include "DisplayXRPlatform.h"
#include "Widgets/SViewport.h"
#include "UnrealEngine.h"
#include "DynamicRHI.h"
#include "SceneView.h"
#include "Camera/CameraTypes.h"

// RDG primitives used by RenderTexture_RenderThread preview blit.
#include "CommonRenderResources.h"   // FCopyRectPS
#include "PixelShaderUtils.h"        // FPixelShaderUtils::AddFullscreenPass
#include "RenderGraphUtils.h"        // AddCopyTexturePass, AddClearRenderTargetPass

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

extern "C" {
#include "Native/display3d_view.h"
#include "Native/camera3d_view.h"
}

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
	static bool bLogged = false;
	if (!bLogged) { bLogged = true; UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] IsStereoEnabled first-call -> true"), WorldCtxTag()); GLog->Flush(); }
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
	CacheWindowSize();
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
	static int32 SUSCount = 0;
	++SUSCount;
	if (SUSCount <= 3 || SUSCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] ShouldUseSeparateRenderTarget #%d -> true"), WorldCtxTag(), SUSCount);
		GLog->Flush();
	}
	return true;
}

bool FDisplayXRDevice::AllocateRenderTargetTexture(uint32 Index, uint32 SizeX, uint32 SizeY, uint8 Format,
	uint32 NumMips, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags,
	FTextureRHIRef& OutTargetableTexture, FTextureRHIRef& OutShaderResourceTexture, uint32 NumSamples)
{
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
	static int32 ARTCount = 0;
	++ARTCount;
	const bool bShouldLog = (ARTCount <= 5);

	if (!Compositor.IsValid() || !Compositor->IsReady())
	{
		if (bShouldLog)
		{
			UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] AllocateRenderTargetTextures #%d early-return: compositor=%s ready=%d (%ux%u)"),
				WorldCtxTag(), ARTCount,
				Compositor.IsValid() ? TEXT("valid") : TEXT("null"),
				(Compositor.IsValid() && Compositor->IsReady()) ? 1 : 0,
				SizeX, SizeY);
			GLog->Flush();
		}
		// Fall back to UE's default / singular allocator until compositor is ready.
		return false;
	}

	TArray<FTextureRHIRef> Wrapped;
	if (!Compositor->GetSwapchainImagesRHI(Wrapped) || Wrapped.Num() == 0)
	{
		if (bShouldLog)
		{
			UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] AllocateRenderTargetTextures #%d early-return: swapchain images empty"),
				WorldCtxTag(), ARTCount);
			GLog->Flush();
		}
		return false;
	}

	OutTargetableTextures = Wrapped;
	OutShaderResourceTextures = Wrapped;

	UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] AllocateRenderTargetTextures #%d -> %d swapchain images (%ux%u)"),
		WorldCtxTag(), ARTCount, Wrapped.Num(), SizeX, SizeY);
	GLog->Flush();
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
	const uint32 InX = InOutSizeX;
	const uint32 InY = InOutSizeY;

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

	static bool bLogged = false;
	static uint32 LastX = 0, LastY = 0;
	if (!bLogged || InOutSizeX != LastX || InOutSizeY != LastY)
	{
		bLogged = true;
		LastX = InOutSizeX;
		LastY = InOutSizeY;
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] CalculateRenderTargetSize: in=%ux%u -> out=%ux%u (compositorReady=%d)"),
			WorldCtxTag(), InX, InY, InOutSizeX, InOutSizeY,
			(Compositor.IsValid() && Compositor->IsReady()) ? 1 : 0);
		GLog->Flush();
	}
}

bool FDisplayXRDevice::NeedReAllocateViewportRenderTarget(const FViewport& Viewport)
{
	static bool bLoggedFirst = false;
	if (!bLoggedFirst)
	{
		bLoggedFirst = true;
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] NeedReAllocateViewportRenderTarget first-call"), WorldCtxTag());
		GLog->Flush();
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
	if (GameHWND)
	{
		RECT rc = {};
		if (::GetClientRect((HWND)GameHWND, &rc))
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
	static bool bLoggedFirst = false;
	static bool bLastSep = false;
	if (!bLoggedFirst || bLastSep != bUseSeparateRenderTarget)
	{
		const FIntPoint VS = Viewport.GetSizeXY();
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] UpdateViewport %s: bSep=%d viewportSize=%dx%d widget=%p"),
			WorldCtxTag(),
			bLoggedFirst ? TEXT("change") : TEXT("first-call"),
			bUseSeparateRenderTarget ? 1 : 0, VS.X, VS.Y, ViewportWidget);
		GLog->Flush();
		bLoggedFirst = true;
		bLastSep = bUseSeparateRenderTarget;
	}

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

	// Deferred compositor creation: create the compositor (which owns session
	// creation with graphics binding) once we have D3D device and game window HWND.
	static bool bCompositorCreationAttempted = false;
	if (Session && !Compositor && !bCompositorCreationAttempted)
	{
		bCompositorCreationAttempted = true;

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

void FDisplayXRDevice::RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer,
	FRDGTextureRef SrcTexture, FVector2f WindowSize) const
{
	static int32 RTCount = 0;
	RTCount++;
	if (RTCount <= 3)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] RenderTexture_RenderThread #%d: BackBuffer=%p SrcTexture=%p"),
			WorldCtxTag(), RTCount, (void*)BackBuffer, (void*)SrcTexture);
		GLog->Flush();
	}

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
	static bool bLogged = false;
	if (!bLogged) { bLogged = true; UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] PostRenderViewFamily_RenderThread first-call"), WorldCtxTag()); GLog->Flush(); }

	if (!Compositor.IsValid() || !Compositor->IsReady()) return;

	FDisplayXRCompositor* Comp = Compositor.Get();

	// Grab the scene color (swapchain image we rendered into) via the view family's render target.
	FRHITexture* SrcTextureRHI = nullptr;
	if (InViewFamily.RenderTarget)
	{
		SrcTextureRHI = InViewFamily.RenderTarget->GetRenderTargetTexture();
	}

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

	static int32 SVFCount = 0;
	SVFCount++;

	Session->Tick();

	// Tick compositor for deferred swapchain creation (needs session running)
	if (Compositor)
	{
		if (SVFCount <= 3 || SVFCount % 300 == 0)
		{
			UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] SetupViewFamily #%d — compositor=%p ready=%d sessionRunning=%d"),
				WorldCtxTag(), SVFCount, Compositor.Get(), Compositor->IsReady() ? 1 : 0, Session->IsSessionRunning() ? 1 : 0);
			GLog->Flush();
		}
		Compositor->Tick();
	}
	else if (SVFCount <= 3)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] SetupViewFamily #%d — no compositor yet"), WorldCtxTag(), SVFCount);
		GLog->Flush();
	}

	CachedViewConfig = Session->GetViewConfig();
	ComputeViews();

	if (SVFCount <= 3)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] SetupViewFamily #%d done — viewConfig %dx%d views=%d"),
			WorldCtxTag(), SVFCount, CachedViewConfig.GetAtlasW(), CachedViewConfig.GetAtlasH(), CachedViewConfig.GetViewCount());
		GLog->Flush();
	}
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
	static bool bLogged = false;
	if (!bLogged) { bLogged = true; UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] BeginRenderViewFamily first-call"), WorldCtxTag()); GLog->Flush(); }
}

void FDisplayXRDevice::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
	static bool bLogged = false;
	if (!bLogged) { bLogged = true; UE_LOG(LogDisplayXRDevice, Log, TEXT("[%s] PreRenderView_RenderThread first-call"), WorldCtxTag()); GLog->Flush(); }
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

	// Eye positions stored as FVector but in OpenXR convention (x,y,z direct from runtime)
	XrVector3f RawEyes[2];
	RawEyes[0] = { (float)LeftEyeRaw.X, (float)LeftEyeRaw.Y, (float)LeftEyeRaw.Z };
	RawEyes[1] = { (float)RightEyeRaw.X, (float)RightEyeRaw.Y, (float)RightEyeRaw.Z };

	// -----------------------------------------------------------------------
	// Window-relative Kooima (matches DisplayXRPreviewSession + reference
	// cube_handle_d3d11_win test app):
	// - Screen dimensions = physical window size in meters (not full display)
	// - Eye positions offset by window-center-minus-monitor-center, so the
	//   off-axis frustum tracks the window across the display as it moves/resizes.
	// -----------------------------------------------------------------------
	const float DispW_m = DI.DisplayWidthMeters  > 0.0f ? DI.DisplayWidthMeters  : 0.344f;
	const float DispH_m = DI.DisplayHeightMeters > 0.0f ? DI.DisplayHeightMeters : 0.194f;
	const float DispPxW = DI.DisplayPixelWidth   > 0    ? (float)DI.DisplayPixelWidth  : 3840.0f;
	const float DispPxH = DI.DisplayPixelHeight  > 0    ? (float)DI.DisplayPixelHeight : 2160.0f;
	const float PxSizeX = DispW_m / DispPxW;
	const float PxSizeY = DispH_m / DispPxH;

	Display3DScreen Screen;
	Screen.width_m  = DispW_m;
	Screen.height_m = DispH_m;
	float EyeOffsetX_m = 0.0f;
	float EyeOffsetY_m = 0.0f;

#if PLATFORM_WINDOWS
	if (GameHWND)
	{
		HWND Hwnd = (HWND)GameHWND;

		RECT rc;
		GetClientRect(Hwnd, &rc);
		const float WinPxW = (float)(rc.right - rc.left);
		const float WinPxH = (float)(rc.bottom - rc.top);
		if (WinPxW > 0.0f && WinPxH > 0.0f)
		{
			Screen.width_m  = WinPxW * PxSizeX;
			Screen.height_m = WinPxH * PxSizeY;
		}

		POINT ClientOrigin = {0, 0};
		ClientToScreen(Hwnd, &ClientOrigin);
		HMONITOR hMon = MonitorFromWindow(Hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { sizeof(mi) };
		if (GetMonitorInfo(hMon, &mi))
		{
			const float WinCenterX = (float)(ClientOrigin.x - mi.rcMonitor.left) + WinPxW * 0.5f;
			const float WinCenterY = (float)(ClientOrigin.y - mi.rcMonitor.top)  + WinPxH * 0.5f;
			const float MonW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
			const float MonH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);

			EyeOffsetX_m =  (WinCenterX - MonW * 0.5f) * PxSizeX;
			EyeOffsetY_m = -(WinCenterY - MonH * 0.5f) * PxSizeY; // screen Y-down → OpenXR Y-up
		}
	}
#endif

	// Shift raw eyes into window-relative display space (still meters, OpenXR convention).
	RawEyes[0].x -= EyeOffsetX_m; RawEyes[0].y -= EyeOffsetY_m;
	RawEyes[1].x -= EyeOffsetX_m; RawEyes[1].y -= EyeOffsetY_m;

	// Nominal viewer position
	XrVector3f NominalViewer = {
		(float)DI.NominalViewerPosition.X,
		(float)DI.NominalViewerPosition.Y,
		(float)DI.NominalViewerPosition.Z
	};
	if (!DI.bIsValid)
	{
		NominalViewer = {0.0f, 0.0f, 0.5f};
	}
	// Keep nominal in the same window-relative frame as RawEyes.
	NominalViewer.x -= EyeOffsetX_m;
	NominalViewer.y -= EyeOffsetY_m;

	// Scene transform (display pose for Kooima)
	FVector ScenePos;
	FQuat SceneOrient;
	bool bSceneEnabled;
	Session->GetSceneTransform(ScenePos, SceneOrient, bSceneEnabled);

	XrPosef DisplayPose;
	DisplayPose.orientation = {0, 0, 0, 1};
	DisplayPose.position = {0, 0, 0};
	if (bSceneEnabled)
	{
		DisplayPose.position = {(float)ScenePos.X, (float)ScenePos.Y, (float)ScenePos.Z};
		DisplayPose.orientation = {(float)SceneOrient.X, (float)SceneOrient.Y,
		                           (float)SceneOrient.Z, (float)SceneOrient.W};
	}

	// Resize cached views array
	CachedViews.SetNum(ViewCount);

	// Kooima always gets 2 eyes (left/right from xrLocateViews)
	const uint32_t KooimaViewCount = 2;

	if (T.bCameraCentric)
	{
		Camera3DTunables CT;
		CT.ipd_factor = T.IpdFactor;
		CT.parallax_factor = T.ParallaxFactor;
		CT.inv_convergence_distance = T.InvConvergenceDistance;
		CT.half_tan_vfov = T.FovOverride > 0.0f ? FMath::Tan(T.FovOverride * 0.5f) : 0.32491969623f;

		Camera3DView OutViews[2];
		camera3d_compute_views(RawEyes, KooimaViewCount, &NominalViewer, &Screen,
			&CT, &DisplayPose, T.NearZ, T.FarZ, OutViews);

		// Get factored eye positions (IPD + parallax applied) for eye_local computation
		XrVector3f FactoredEyes[2];
		display3d_apply_eye_factors_n(RawEyes, KooimaViewCount, &NominalViewer,
			CT.ipd_factor, CT.parallax_factor, FactoredEyes);

		const float NomZ = (NominalViewer.z > 0.0f) ? NominalViewer.z : 0.5f;

		for (int32 i = 0; i < ViewCount; i++)
		{
			const int32 SrcIdx = FMath::Min(i, (int32)KooimaViewCount - 1);

			// eye_local = processed_eye - (0, 0, nominal_z): displacement from camera center.
			// This is camera-local (unrotated). UE's CalculateStereoViewOffset rotates it
			// by ViewRotation to produce the world-space offset.
			XrVector3f EyeLocal = {
				FactoredEyes[SrcIdx].x,
				FactoredEyes[SrcIdx].y,
				FactoredEyes[SrcIdx].z - NomZ
			};
			CachedViews[i].Offset = OpenXRPositionToUE(EyeLocal);

			// Projection: use same eye_local with inv_convergence scaling (matches Kooima)
			const float ConvergenceDist = T.InvConvergenceDistance > 0.0f
				? (1.0f / T.InvConvergenceDistance) * 100.0f : 100.0f;
			const float AspectRatio = Screen.width_m / Screen.height_m;
			FVector2D HalfSize(ConvergenceDist * CT.half_tan_vfov * AspectRatio,
			                   ConvergenceDist * CT.half_tan_vfov);
			FVector EyeForProj(-ConvergenceDist, CachedViews[i].Offset.Y, CachedViews[i].Offset.Z);
			CachedViews[i].ProjectionMatrix = CalculateOffAxisProjectionMatrix(HalfSize, EyeForProj);
		}
	}
	else
	{
		Display3DTunables DT;
		DT.ipd_factor = T.IpdFactor;
		DT.parallax_factor = T.ParallaxFactor;
		DT.perspective_factor = T.PerspectiveFactor;
		// vdh is FIXED (does not scale with window). Screen.height_m is the
		// window physical height, so m2v = vdh / window_h changes with resize —
		// making world-scale objects physically smaller as the window shrinks
		// (matches test app cube_handle_d3d11_win and DisplayXRPreviewSession).
		DT.virtual_display_height = T.VirtualDisplayHeight > 0.0f
			? T.VirtualDisplayHeight : DispH_m;

		Display3DView OutViews[2];
		display3d_compute_views(RawEyes, KooimaViewCount, &NominalViewer, &Screen,
			&DT, &DisplayPose, T.NearZ, T.FarZ, OutViews);

		for (int32 i = 0; i < ViewCount; i++)
		{
			const int32 SrcIdx = FMath::Min(i, (int32)KooimaViewCount - 1);

			// eye_display: post-factor eye in display space (meters, OpenXR convention)
			// Matches Unity/test app: camera moves to eye position, projection from same.
			FVector EyeDisplayUE = OpenXRPositionToUE(OutViews[SrcIdx].eye_display);
			CachedViews[i].Offset = EyeDisplayUE;

			// Screen dimensions in UE units, scaled by m2v (matching Kooima's kScreenW/H).
			// Note: m2v only, no perspective_factor — that only applies to eye position.
			const float m2v = DT.virtual_display_height / Screen.height_m;
			const float ScreenW_UE = Screen.width_m * m2v * 100.0f;
			const float ScreenH_UE = DT.virtual_display_height * 100.0f;
			FVector2D HalfSize(ScreenW_UE * 0.5f, ScreenH_UE * 0.5f);

			// Full eye position for projection — depth comes from actual eye Z, not static nominal
			CachedViews[i].ProjectionMatrix = CalculateOffAxisProjectionMatrix(HalfSize, EyeDisplayUE);
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

	// Center projection from center eye position
	if (!T.bCameraCentric)
	{
		const float NominalZ = DI.bIsValid ? (float)DI.NominalViewerPosition.Z : 0.5f;
		const float VDH = T.VirtualDisplayHeight > 0.0f ? T.VirtualDisplayHeight : Screen.height_m;
		const float ConvergenceDist = NominalZ * T.PerspectiveFactor * (VDH / Screen.height_m) * 100.0f;
		const float VirtualH = VDH * 100.0f;
		const float AspectRatio = Screen.width_m / Screen.height_m;
		FVector2D HalfSize(VirtualH * 0.5f * AspectRatio, VirtualH * 0.5f);
		FVector EyeForProj(-ConvergenceDist, CachedCenter.Offset.Y, CachedCenter.Offset.Z);
		CachedCenter.ProjectionMatrix = CalculateOffAxisProjectionMatrix(HalfSize, EyeForProj);
	}
	else
	{
		const float ConvergenceDist = T.InvConvergenceDistance > 0.0f
			? (1.0f / T.InvConvergenceDistance) * 100.0f : 100.0f;
		const float HalfTanVFOV = T.FovOverride > 0.0f ? FMath::Tan(T.FovOverride * 0.5f) : 0.32491969623f;
		const float AspectRatio = Screen.width_m / Screen.height_m;
		FVector2D HalfSize(ConvergenceDist * HalfTanVFOV * AspectRatio,
		                   ConvergenceDist * HalfTanVFOV);
		FVector EyeForProj(-ConvergenceDist, CachedCenter.Offset.Y, CachedCenter.Offset.Z);
		CachedCenter.ProjectionMatrix = CalculateOffAxisProjectionMatrix(HalfSize, EyeForProj);
	}
}
