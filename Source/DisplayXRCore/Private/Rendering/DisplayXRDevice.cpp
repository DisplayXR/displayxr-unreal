// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRDevice.h"
#include "DisplayXRStereoMath.h"
#include "DisplayXRRigManager.h"
#include "DisplayXRPlatform.h"
#include "Widgets/SViewport.h"
#include "UnrealEngine.h"
#include "DynamicRHI.h"

extern "C" {
#include "Native/display3d_view.h"
#include "Native/camera3d_view.h"
}

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRDevice, Log, All);

// =============================================================================
// Constructor
// =============================================================================

FDisplayXRDevice::FDisplayXRDevice(const FAutoRegister& AutoRegister, FDisplayXRSession* InSession)
	: FHeadMountedDisplayBase(nullptr)
	, FSceneViewExtensionBase(AutoRegister)
	, Session(InSession)
{
	UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR Device: Created"));
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
	return true;
}

void FDisplayXRDevice::AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const
{
	// Each tile is ScaleX * DisplayW wide, ScaleY * DisplayH tall.
	// Tiles are laid out in a TileColumns x TileRows grid starting at top-left.
	const int32 TileW = CachedViewConfig.GetTileW();
	const int32 TileH = CachedViewConfig.GetTileH();
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

	UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR: AllocateRenderTargetTexture (fallback) %ux%u fmt=%d"),
		SizeX, SizeY, (int)DesiredFormat);

	return OutTargetableTexture.IsValid();
}

bool FDisplayXRDevice::AllocateRenderTargetTextures(uint32 SizeX, uint32 SizeY, uint8 Format,
	uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags,
	TArray<FTextureRHIRef>& OutTargetableTextures,
	TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples)
{
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

	UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR: AllocateRenderTargetTextures -> %d swapchain images (%ux%u)"),
		Wrapped.Num(), SizeX, SizeY);
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
			return;
		}
	}

	InOutSizeX = CachedViewConfig.GetAtlasW();
	InOutSizeY = CachedViewConfig.GetAtlasH();
	if (InOutSizeX == 0) InOutSizeX = 1920;
	if (InOutSizeY == 0) InOutSizeY = 1080;
}

bool FDisplayXRDevice::NeedReAllocateViewportRenderTarget(const FViewport& Viewport)
{
	// One-shot reallocation trigger when the compositor transitions to ready,
	// so UE re-runs AllocateRenderTargetTextures with the swapchain-backed array.
	if (Compositor.IsValid() && Compositor->IsReady() && bSwapchainRTReallocPending)
	{
		bSwapchainRTReallocPending = false;
		return true;
	}

	const FIntPoint RenderTargetSize = Viewport.GetRenderTargetTextureSizeXY();
	uint32 NewSizeX, NewSizeY;
	CalculateRenderTargetSize(Viewport, NewSizeX, NewSizeY);
	return (NewSizeX != (uint32)RenderTargetSize.X || NewSizeY != (uint32)RenderTargetSize.Y);
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

	// Deferred compositor creation: create the compositor (which owns session
	// creation with graphics binding) once we have D3D device and game window HWND.
	static bool bCompositorCreationAttempted = false;
	if (Session && !Compositor && !bCompositorCreationAttempted)
	{
		bCompositorCreationAttempted = true;

		// Get HWND from viewport widget chain
		void* WindowHandle = nullptr;
		TSharedPtr<SWidget> WindowWidget = GetTopMostWidget(ViewportWidget->GetParentWidget());
		if (WindowWidget.IsValid())
		{
			SWindow* WindowPtr = static_cast<SWindow*>(WindowWidget.Get());
			WindowHandle = WindowPtr->GetNativeWindow()->GetOSWindowHandle();
		}

		// Get D3D device and command queue from UE's RHI
		void* D3DDevice = GDynamicRHI ? GDynamicRHI->RHIGetNativeDevice() : nullptr;
		void* CommandQueue = GDynamicRHI ? GDynamicRHI->RHIGetNativeGraphicsQueue() : nullptr;

		if (D3DDevice && WindowHandle)
		{
			Compositor = MakeUnique<FDisplayXRCompositor>(Session);
			if (!Compositor->Initialize(WindowHandle, D3DDevice, CommandQueue))
			{
				UE_LOG(LogDisplayXRDevice, Warning, TEXT("DisplayXR Device: Compositor initialization failed"));
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
		UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR RenderTexture #%d: BackBuffer=%p SrcTexture=%p"),
			RTCount, (void*)BackBuffer, (void*)SrcTexture);
		GLog->Flush();
	}

	// Zero-copy path: SrcTexture IS the swapchain image UE rendered into.
	// The OpenXR compositor owns display output, so skip the backbuffer blit.
	// Swapchain release happens in PostRenderViewFamily_RenderThread, which is
	// the SceneViewExtension hook that runs after the scene's RDG graph flushes.
}

void FDisplayXRDevice::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
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
			UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR Device: SetupViewFamily #%d — compositor=%p ready=%d sessionRunning=%d"),
				SVFCount, Compositor.Get(), Compositor->IsReady() ? 1 : 0, Session->IsSessionRunning() ? 1 : 0);
			GLog->Flush();
		}
		Compositor->Tick();
	}
	else if (SVFCount <= 3)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR Device: SetupViewFamily #%d — no compositor yet"), SVFCount);
		GLog->Flush();
	}

	CachedViewConfig = Session->GetViewConfig();
	ComputeViews();

	if (SVFCount <= 3)
	{
		UE_LOG(LogDisplayXRDevice, Log, TEXT("DisplayXR Device: SetupViewFamily #%d done — viewConfig %dx%d views=%d"),
			SVFCount, CachedViewConfig.GetAtlasW(), CachedViewConfig.GetAtlasH(), CachedViewConfig.GetViewCount());
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
	return GEngine && GEngine->IsStereoscopic3D(Context.Viewport);
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

	// Screen dimensions
	Display3DScreen Screen;
	Screen.width_m = DI.DisplayWidthMeters > 0.0f ? DI.DisplayWidthMeters : 0.15f;
	Screen.height_m = DI.DisplayHeightMeters > 0.0f ? DI.DisplayHeightMeters : 0.09f;

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
		DT.virtual_display_height = T.VirtualDisplayHeight > 0.0f
			? T.VirtualDisplayHeight : Screen.height_m;

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
