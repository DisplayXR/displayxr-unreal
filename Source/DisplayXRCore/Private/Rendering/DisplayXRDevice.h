// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "HeadMountedDisplayBase.h"
#include "XRRenderTargetManager.h"
#include "SceneViewExtension.h"
#include "DisplayXRSession.h"
#include "DisplayXRCompositor.h"
#include "RenderGraphBuilder.h"

/**
 * Custom HMD device for DisplayXR.
 *
 * Owns the HMD role via FHeadMountedDisplayBase so UE calls our
 * GetStereoProjectionMatrix, CalculateStereoViewOffset, AdjustViewRect at the
 * right seams -- before its own view construction runs. This eliminates all the
 * convention-fighting that plagued the OpenXR hook approach.
 *
 * Each frame in SetupViewFamily:
 *   1. Tick the session, which chains an XR_DXR_view_rig descriptor onto
 *      xrLocateViews so the RUNTIME owns the view math (#396 W7, ADR-024)
 *   2. Consume the render-ready XrView{pose, fov} it returns
 *   3. Convert fov to a UE reverse-Z projection (ProjectionMatrixFromFov);
 *      near/far and the depth convention stay app-side
 *   4. Cache per-view matrices and offsets for IStereoRendering overrides
 *
 * N-view atlas layout built on FHeadMountedDisplayBase + FXRRenderTargetManager.
 */
class FDisplayXRDevice
	: public FHeadMountedDisplayBase
	, public FXRRenderTargetManager
	, public FSceneViewExtensionBase
{
public:
	FDisplayXRDevice(const FAutoRegister& AutoRegister, FDisplayXRSession* InSession);
	virtual ~FDisplayXRDevice();

	// --- IXRTrackingSystem ---
	virtual FName GetSystemName() const override;
	virtual int32 GetXRSystemFlags() const override;
	virtual bool EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type) override;
	virtual bool GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition) override;
	virtual float GetWorldToMetersScale() const override;
	virtual void ResetOrientationAndPosition(float Yaw) override;
	virtual void OnBeginPlay(FWorldContext& InWorldContext) override;

	// --- IHeadMountedDisplay ---
	virtual bool IsHMDConnected() override;
	virtual bool IsHMDEnabled() const override;
	virtual void EnableHMD(bool bEnable) override;
	virtual bool GetHMDMonitorInfo(MonitorInfo& MonitorDesc) override;
	virtual void GetFieldOfView(float& InOutHFOVInDegrees, float& InOutVFOVInDegrees) const override;
	virtual void SetInterpupillaryDistance(float NewInterpupillaryDistance) override;
	virtual float GetInterpupillaryDistance() const override;
	virtual bool IsChromaAbCorrectionEnabled() const override;
	virtual IHeadMountedDisplay* GetHMDDevice() override;
	virtual bool GetHMDDistortionEnabled(EShadingPath ShadingPath) const override;

	// --- IStereoRendering ---
	virtual bool IsStereoEnabled() const override;
	virtual bool EnableStereo(bool stereo) override;
	virtual void AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const override;
	virtual FMatrix GetStereoProjectionMatrix(const int32 ViewIndex) const override;
	virtual void CalculateStereoViewOffset(const int32 ViewIndex, FRotator& ViewRotation,
		const float WorldToMeters, FVector& ViewLocation) override;
	virtual int32 GetDesiredNumberOfViews(bool bStereoRequested) const override;
	virtual TSharedPtr<IStereoRendering, ESPMode::ThreadSafe> GetStereoRenderingDevice() override;
	virtual IStereoRenderTargetManager* GetRenderTargetManager() override;

	// --- FXRRenderTargetManager ---
	virtual bool ShouldUseSeparateRenderTarget() const override;
	virtual void CalculateRenderTargetSize(const FViewport& Viewport, uint32& InOutSizeX, uint32& InOutSizeY) override;
	virtual bool NeedReAllocateViewportRenderTarget(const FViewport& Viewport) override;
	virtual void UpdateViewport(bool bUseSeparateRenderTarget, const FViewport& Viewport, SViewport* ViewportWidget) override;
	virtual bool AllocateRenderTargetTexture(uint32 Index, uint32 SizeX, uint32 SizeY, uint8 Format,
		uint32 NumMips, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags,
		FTextureRHIRef& OutTargetableTexture, FTextureRHIRef& OutShaderResourceTexture,
		uint32 NumSamples = 1) override;
	virtual bool AllocateRenderTargetTextures(uint32 SizeX, uint32 SizeY, uint8 Format,
		uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags,
		TArray<FTextureRHIRef>& OutTargetableTextures,
		TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples = 1) override;
	virtual int32 AcquireColorTexture() override;
	virtual EPixelFormat GetActualColorSwapchainFormat() const override;
	virtual void RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer,
		FRDGTextureRef SrcTexture, FVector2f WindowSize) const override;

	// --- FSceneViewExtensionBase ---
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void SetupViewPoint(APlayerController* Player, FMinimalViewInfo& InViewInfo) override;
	virtual void SetupViewProjectionMatrix(FSceneViewProjectionData& InOutProjectionData) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual int32 GetPriority() const override;
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	using FSceneViewExtensionBase::IsActiveThisFrame;

	/**
	 * Destroy the compositor at the end of a play session, while the window it
	 * is bound to is still alive.
	 *
	 * Deliberately leaves compositor creation DISARMED: a trailing
	 * UpdateViewport during teardown would otherwise rebuild a compositor bound
	 * to a dying window. RearmCompositorCreation() re-opens creation when the
	 * next session starts. Safe to call when no compositor exists.
	 *
	 * Flushes rendering first: UE holds the OpenXR swapchain images as its
	 * viewport render targets (AllocateRenderTargetTextures hands them out
	 * zero-copy), so the compositor must not destroy the swapchain while the
	 * render thread may still touch those textures.
	 */
	void ShutdownCompositorForSessionEnd();

	/**
	 * Re-open deferred compositor creation for a new play session.
	 *
	 * Creation is one-shot per arming. Without this re-arm the second editor PIE
	 * session in a process would never build a compositor and stereo would
	 * silently stay off — the guard used to be a function-local static, so it
	 * latched for the whole editor run.
	 */
	void RearmCompositorCreation();

	/** The compositor, or null before deferred creation / after session-end
	 *  teardown. Game thread only — teardown resets it on the game thread. */
	FDisplayXRCompositor* GetCompositor() const { return Compositor.Get(); }

private:
	void ComputeViews();

	FDisplayXRSession* Session = nullptr;
	TUniquePtr<FDisplayXRCompositor> Compositor;
	FVector PlayerViewLocation_GameThread = FVector::ZeroVector;

	// Per-view cached data (computed in SetupViewFamily, consumed by IStereoRendering)
	struct FPerViewData
	{
		FMatrix ProjectionMatrix = FMatrix::Identity;
		FVector Offset = FVector::ZeroVector;
	};
	TArray<FPerViewData> CachedViews;
	FPerViewData CachedCenter;
	FDisplayXRViewConfig CachedViewConfig;

	// One-shot reallocation trigger: fires true once when compositor becomes ready
	// so UE re-runs AllocateRenderTargetTextures with the new (swapchain) size.
	mutable bool bSwapchainRTReallocPending = true;

	// Guards the deferred one-shot compositor creation in UpdateViewport. A
	// member, not a function-local static: a static is process-lifetime, so the
	// second PIE session in an editor run could never rebuild the compositor.
	// RearmCompositorCreation() clears it.
	bool bCompositorCreationAttempted = false;

	// Host game window HWND, re-cached each UpdateViewport. Used per-frame in
	// ComputeViews to compute window-relative Kooima inputs (eye offset + screen
	// dims) so the off-axis frustum tracks the window as it moves/resizes.
	void* GameHWND = nullptr;

	// Cached window client-area pixel dims, refreshed by CacheWindowSize().
	// Read by const methods (AdjustViewRect, CalculateRenderTargetSize,
	// RenderTexture_RenderThread), hence mutable.
	mutable uint32 CachedWindowW = 0;
	mutable uint32 CachedWindowH = 0;

	// Query HWND client rect into the cached window dims. Falls back to the
	// DisplayXR panel pixel dims when HWND is stale / off-screen / platform-
	// unavailable, so the plugin still produces a valid content region.
	void CacheWindowSize() const;

	// --- 2D UI per eye tile (game path) — see Docs/DisplayXR/UICompositing.md ---

	// Latched from r.DisplayXR.UIPerEyeTiles at construction.
	bool bUIPerEyeTiles = true;

	// True when this frame's atlas goes through UE's own render target so the
	// window UI Slate paints can be composited into every eye tile: game path
	// only (editor texture mode and the IPC array-copy path keep their flows).
	bool UsesUIPerEyeTiles() const;

	// Atlas-target size for the per-eye UI path: the viewport's own size, clamped
	// to the swapchain. Deliberately not CacheWindowSize() — see the definition.
	bool GetUITargetSize(const FViewport& Viewport, uint32& OutW, uint32& OutH) const;

	// Render-thread hand-off between PostRenderViewFamily (atlas copied into an
	// acquired swapchain image, UE's RT cleared for Slate) and
	// RenderTexture_RenderThread (UI blended into each tile, image released).
	struct FPendingUIComposite
	{
		FTextureRHIRef Swapchain;
		TArray<FIntRect> TileRects;
		bool IsValid() const { return Swapchain.IsValid(); }
		void Reset() { Swapchain = nullptr; TileRects.Reset(); }
	};
	mutable FPendingUIComposite PendingUI_RT;
};
