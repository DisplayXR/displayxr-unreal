// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

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
 *   1. Tick the session (poll OpenXR, update eye positions)
 *   2. Feed raw eyes to Kooima C library (display3d/camera3d_compute_views)
 *   3. Consume eye_display output (post-factor eye position in display space)
 *   4. Build UE-native off-axis projection via CalculateOffAxisProjectionMatrix
 *   5. Cache per-view matrices and offsets for IStereoRendering overrides
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
};
