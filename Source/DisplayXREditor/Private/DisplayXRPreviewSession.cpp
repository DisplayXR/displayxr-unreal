// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRPreviewSession.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/World.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditor.h"
#include "IAssetViewport.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "Framework/Application/SlateApplication.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
#include "DisplayXRCamera.h"
#include "DisplayXRDisplay.h"
#include "EngineUtils.h"
#include "RHICommandList.h"
#include "Async/Async.h"

#if PLATFORM_WINDOWS
#include "ID3D12DynamicRHI.h"
#endif

// Stereo math helpers (OpenXR ↔ UE coordinate conversion, off-axis projection)
// All static inline — no link dependency on DisplayXRCore internals
#include "DisplayXRStereoMath.h"

// Kooima view library — include the .c implementation directly since
// display3d_compute_views is compiled into DisplayXRCore but not exported.
// The header has extern "C" guards so this is safe from C++ name mangling.
#include "display3d_view.c"
#include "camera3d_view.c"

// D3D12 swapchain image struct (not in bundled openxr.h)
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR_VALUE ((XrStructureType)1000028001)
struct XrSwapchainImageD3D12KHR { XrStructureType type; void* next; void* texture; };

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRPreviewSession, Log, All);

// =============================================================================
// Native preview window (matches Unity's displayxr_standalone.cpp pattern)
// The runtime renders interlaced 3D into this window.
// =============================================================================
#if PLATFORM_WINDOWS
static FDisplayXRPreviewSession* GPreviewSessionForWndProc = nullptr;

// Captured once when the preview window is created so we can restore focus to
// the editor. Keyboard events that land on the preview are forwarded here.
static HWND GPreviewEditorHWND = nullptr;

// LMB-drag state for mouse-look forwarding. Tracked in the WndProc since mouse
// events are Win32-native.
static POINT GDragLastCursor = {0, 0};
static bool GDragging = false;

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_MOUSEACTIVATE:
		// Clicks on the preview must not activate it or steal focus from the
		// editor's PIE viewport. MA_NOACTIVATE tells Windows to deliver the
		// click without activation.
		return MA_NOACTIVATE;
	case WM_LBUTTONDOWN:
		// LMB-down: focus PIE for keyboard, start tracking a potential
		// drag. If the user drags, we forward deltas as rotation input.
		if (GPreviewSessionForWndProc)
		{
			GPreviewSessionForWndProc->FocusPIEViewport();
		}
		GetCursorPos(&GDragLastCursor);
		GDragging = true;
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONUP:
		if (GDragging)
		{
			GDragging = false;
			ReleaseCapture();
		}
		return 0;
	case WM_MOUSEMOVE:
		if (GDragging && GPreviewSessionForWndProc)
		{
			POINT Current;
			GetCursorPos(&Current);
			const int dx = Current.x - GDragLastCursor.x;
			const int dy = Current.y - GDragLastCursor.y;
			if (dx != 0 || dy != 0)
			{
				GPreviewSessionForWndProc->ForwardMouseLookDelta(dx, dy);
				GDragLastCursor = Current;
			}
		}
		break;
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
		// Other buttons just focus PIE so their bindings (attack, interact,
		// etc.) can fire via keystrokes that follow.
		if (GPreviewSessionForWndProc)
		{
			GPreviewSessionForWndProc->FocusPIEViewport();
		}
		return 0;
	case WM_MOVE:
	case WM_SIZE:
		if (GPreviewSessionForWndProc)
			GPreviewSessionForWndProc->UpdateCanvasRect();
		break;
	case WM_KEYDOWN:
		// ESC closes the preview and stops PIE — kept as a local handler so
		// it works even if the preview is the only window receiving the key
		// (e.g., if Windows dialog focus-cycling routes to us).
		if (wParam == VK_ESCAPE && GPreviewSessionForWndProc)
		{
			GPreviewSessionForWndProc->OnNativeWindowClosed();
			return 0;
		}
		// Everything else (including TAB) is forwarded to the editor so the
		// game's bindings handle it. Auto-follow keeps the preview in sync
		// with any rig / view-target switch the game performs.
		if (GPreviewEditorHWND && IsWindow(GPreviewEditorHWND))
		{
			::PostMessageW(GPreviewEditorHWND, msg, wParam, lParam);
			return 0;
		}
		break;
	case WM_KEYUP:
	case WM_CHAR:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
		if (GPreviewEditorHWND && IsWindow(GPreviewEditorHWND))
		{
			::PostMessageW(GPreviewEditorHWND, msg, wParam, lParam);
			return 0;
		}
		break;
	case WM_CLOSE:
		if (GPreviewSessionForWndProc)
		{
			GPreviewSessionForWndProc->OnNativeWindowClosed();
			return 0; // Don't call DestroyWindow — Stop() handles cleanup
		}
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool bPreviewClassReg = false;
static bool RegPreviewWindowClass()
{
	if (bPreviewClassReg) return true;
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = PreviewWndProc;
	wc.hInstance = GetModuleHandleW(0);
	wc.lpszClassName = L"DisplayXRPreview";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
	bPreviewClassReg = true;
	return true;
}
#endif

// The preview lifecycle is driven by FEditorDelegates::BeginPIE / EndPIE, so
// we always want the PIE world — that's where gameplay runs. Capturing the
// editor world would show a frozen scene during Play. Matches the Unity
// plugin's Play behavior. PlayWorld is not set at BeginPIE time; the caller
// handles the null case via lazy retry in Tick().
static UWorld* GetPreviewTargetWorld()
{
	return GEditor ? GEditor->PlayWorld : nullptr;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

FDisplayXRPreviewSession::FDisplayXRPreviewSession()
{
}

FDisplayXRPreviewSession::~FDisplayXRPreviewSession()
{
	if (bActive)
	{
		Stop();
	}
}

// =============================================================================
// Start
// =============================================================================

bool FDisplayXRPreviewSession::Start()
{
	if (bActive)
	{
		return true;
	}

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Starting standalone session..."));

	// 1. Load runtime DLL and get xrGetInstanceProcAddr
	if (!LoadOpenXRRuntime())
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to load OpenXR runtime"));
		return false;
	}

	// 2. Create XrInstance + get system + query display info
	if (!CreateXrInstance())
	{
		UnloadOpenXRRuntime();
		return false;
	}
	QueryDisplayInfo();

	// 3. Get D3D12 device from UE's RHI
#if PLATFORM_WINDOWS
	D3DDevice = GDynamicRHI ? GDynamicRHI->RHIGetNativeDevice() : nullptr;
	if (!D3DDevice)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to get D3D12 device from RHI"));
		DestroyXrInstance();
		UnloadOpenXRRuntime();
		return false;
	}

	// 4. Create dedicated command queue for the runtime
	if (!CreateRuntimeQueue())
	{
		DestroyXrInstance();
		UnloadOpenXRRuntime();
		return false;
	}
#endif

	// 5. Resolve frame loop function pointers (needs instance)
	if (!ResolveXrFunctions())
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to resolve XR functions"));
#if PLATFORM_WINDOWS
		if (RuntimeQueue) { static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release(); RuntimeQueue = nullptr; }
#endif
		DestroyXrInstance();
		UnloadOpenXRRuntime();
		return false;
	}

	// 5b. Create native preview window
	if (!CreateNativeWindow())
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to create native window"));
#if PLATFORM_WINDOWS
		if (RuntimeQueue) { static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release(); RuntimeQueue = nullptr; }
#endif
		DestroyXrInstance();
		UnloadOpenXRRuntime();
		return false;
	}

	// 6. Create XrSession with D3D12 + Win32 bindings
	if (!CreateXrSession())
	{
#if PLATFORM_WINDOWS
		if (RuntimeQueue) { static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release(); RuntimeQueue = nullptr; }
#endif
		DestroyXrInstance();
		UnloadOpenXRRuntime();
		return false;
	}

	// 7. Poll events until READY, then xrBeginSession
	{
		PFN_xrPollEvent xrPollEventFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrPollEvent", (PFN_xrVoidFunction*)&xrPollEventFunc);
		PFN_xrBeginSession xrBeginSessionFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrBeginSession", (PFN_xrVoidFunction*)&xrBeginSessionFunc);

		if (xrPollEventFunc && xrBeginSessionFunc)
		{
			for (int i = 0; i < 100; i++)
			{
				XrEventDataBuffer EventData = {XR_TYPE_EVENT_DATA_BUFFER};
				XrResult r = xrPollEventFunc(Instance, &EventData);
				if (!XR_SUCCEEDED(r)) break;

				if (EventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					auto* SC = (XrEventDataSessionStateChanged*)&EventData;
					if (SC->state == XR_SESSION_STATE_READY)
					{
						XrSessionBeginInfo BeginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
						BeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						XrResult BeginResult = xrBeginSessionFunc(Session, &BeginInfo);
						if (XR_SUCCEEDED(BeginResult))
						{
							bSessionRunning = true;
							UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Session running"));
						}
						else
						{
							UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: xrBeginSession failed (%d)"), (int)BeginResult);
						}
						break;
					}
				}
			}
		}

		if (!bSessionRunning)
		{
			UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Session did not reach READY state"));
			DestroyXrSession();
#if PLATFORM_WINDOWS
			if (RuntimeQueue) { static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release(); RuntimeQueue = nullptr; }
#endif
			DestroyXrInstance();
			UnloadOpenXRRuntime();
			return false;
		}
	}

	// 8. Query rendering modes (needs running session for some runtimes)
	QueryRenderingModes();

	// 9. Create swapchain
	if (!CreateSwapchain())
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to create swapchain"));
		Stop();
		return false;
	}

	// 10. Create scene capture components
	if (!CreateSceneCaptures())
	{
		UE_LOG(LogDisplayXRPreviewSession, Warning, TEXT("DisplayXR Preview: Failed to create scene captures — submitting empty frames"));
	}

	// 12. Scan for DisplayXR rig components in the scene
	ScanForRigs();

	bActive = true;
	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Session started — swapchain %dx%d, %d images, captures=%s"),
		SwapchainWidth, SwapchainHeight, SwapchainImages.Num(),
		LeftCapture ? TEXT("yes") : TEXT("no"));
	return true;
}

// =============================================================================
// Stop
// =============================================================================

void FDisplayXRPreviewSession::Stop()
{
	if (!bActive && !bSessionRunning && Instance == XR_NULL_HANDLE)
	{
		return;
	}

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Stopping session..."));

	// Destroy scene captures first (they reference the editor world)
	DestroySceneCaptures();
	SwapchainImagesRHI.Empty();
	bSwapchainWrapped = false;

	// End session
	if (bSessionRunning && Session != XR_NULL_HANDLE)
	{
		PFN_xrEndSession xrEndSessionFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrEndSession", (PFN_xrVoidFunction*)&xrEndSessionFunc);
		if (xrEndSessionFunc)
		{
			xrEndSessionFunc(Session);
		}
		bSessionRunning = false;
	}

	DestroySwapchain();
	DestroyXrSession();
	DestroyNativeWindow();

#if PLATFORM_WINDOWS
	if (RuntimeQueue)
	{
		static_cast<ID3D12CommandQueue*>(RuntimeQueue)->Release();
		RuntimeQueue = nullptr;
	}
#endif
	D3DDevice = nullptr;

	DestroyXrInstance();
	UnloadOpenXRRuntime();

	PredictedDisplayTime = 0;
	FrameCount = 0;
	bActive = false;

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Session stopped"));
}

// =============================================================================
// Tick — single-threaded frame loop on editor game thread
// =============================================================================

void FDisplayXRPreviewSession::Tick()
{
	if (!bActive || !bSessionRunning || Session == XR_NULL_HANDLE)
	{
		return;
	}

	// Check if native window was closed (WM_CLOSE or ESC)
	if (bWindowClosed)
	{
		bWindowClosed = false;
		// Request PIE stop on the game thread — the editor module's OnEndPIE handles cleanup
		AsyncTask(ENamedThreads::GameThread, []()
		{
			if (GEditor && GEditor->IsPlayingSessionInEditor())
			{
				GEditor->RequestEndPlayMap();
			}
		});
		return;
	}

	// Update canvas rect every tick (catches resize/move even if WndProc is delayed)
	UpdateCanvasRect();

	// xrWaitFrame (blocks for vsync — acceptable on editor game thread)
	XrFrameWaitInfo WaitInfo = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState FrameState = {XR_TYPE_FRAME_STATE};
	XrResult r = xrWaitFrameFunc(Session, &WaitInfo, &FrameState);
	if (!XR_SUCCEEDED(r))
	{
		if (FrameCount == 0)
		{
			UE_LOG(LogDisplayXRPreviewSession, Warning, TEXT("DisplayXR Preview: xrWaitFrame failed (%d)"), (int)r);
		}
		return;
	}

	PredictedDisplayTime = FrameState.predictedDisplayTime;

	// xrBeginFrame
	XrFrameBeginInfo BeginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
	r = xrBeginFrameFunc(Session, &BeginInfo);
	if (!XR_SUCCEEDED(r))
	{
		return;
	}

	// If runtime says don't render, or no swapchain, submit empty frame
	if (!FrameState.shouldRender || Swapchain == XR_NULL_HANDLE)
	{
		XrFrameEndInfo EndInfo = {XR_TYPE_FRAME_END_INFO};
		EndInfo.displayTime = PredictedDisplayTime;
		EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		xrEndFrameFunc(Session, &EndInfo);
		return;
	}

	// Acquire swapchain image
	uint32_t ImageIndex = 0;
	XrSwapchainImageAcquireInfo AcqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	r = xrAcquireSwapchainImageFunc(Swapchain, &AcqInfo, &ImageIndex);
	if (!XR_SUCCEEDED(r))
	{
		XrFrameEndInfo EndInfo = {XR_TYPE_FRAME_END_INFO};
		EndInfo.displayTime = PredictedDisplayTime;
		EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		xrEndFrameFunc(Session, &EndInfo);
		return;
	}

	XrSwapchainImageWaitInfo SwWaitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	SwWaitInfo.timeout = 100000000LL; // 100ms in nanoseconds
	r = xrWaitSwapchainImageFunc(Swapchain, &SwWaitInfo);
	if (!XR_SUCCEEDED(r))
	{
		XrSwapchainImageReleaseInfo RelInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImageFunc(Swapchain, &RelInfo);
		XrFrameEndInfo EndInfo = {XR_TYPE_FRAME_END_INFO};
		EndInfo.displayTime = PredictedDisplayTime;
		EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		xrEndFrameFunc(Session, &EndInfo);
		return;
	}

	// Lazy init: GEditor->PlayWorld is not yet set when BeginPIE fires, so
	// CreateSceneCaptures/ScanForRigs would have failed in Start(). Retry
	// each tick until the PIE world is available — typically the very next
	// frame after BeginPIE.
	// Defer capture creation until the PIE world has a pawn possessed by a
	// rig with a UDisplayXRCamera / UDisplayXRDisplay component. This avoids
	// showing the wrong rig during the window between BeginPIE and the game
	// Blueprint's possession/rig-switcher code running (can be many frames).
	if (!LeftCapture && GetPreviewTargetWorld())
	{
		UWorld* World = GetPreviewTargetWorld();
		bool bHasControlledRig = false;
		for (TActorIterator<AActor> It(World); It && !bHasControlledRig; ++It)
		{
			AActor* Actor = *It;
			if (!Actor->FindComponentByClass<UDisplayXRCamera>() &&
				!Actor->FindComponentByClass<UDisplayXRDisplay>()) continue;
			if (!Actor->FindComponentByClass<UCameraComponent>()) continue;
			if (APawn* P = Cast<APawn>(Actor))
			{
				if (P->IsLocallyControlled()) bHasControlledRig = true;
			}
		}
		if (bHasControlledRig && CreateSceneCaptures())
		{
			ScanForRigs();
			UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Scene captures lazy-initialized against PIE world"));
		}
	}

	// Auto-follow the player's current view target every frame. The view
	// target is what the main viewport actually renders from — which may
	// differ from the possessed pawn if the game uses SetViewTarget() to
	// switch cameras independently of possession. Matching the view target
	// keeps the preview POV in sync with game-mode rendering.
	if (DiscoveredRigs.Num() > 0)
	{
		AActor* ViewTargetActor = nullptr;
		if (UWorld* World = GetPreviewTargetWorld())
		{
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* PC = It->Get())
				{
					if (AActor* VT = PC->GetViewTarget())
					{
						ViewTargetActor = VT;
						break;
					}
				}
			}
		}
		int32 TargetIdx = -1;
		if (ViewTargetActor)
		{
			for (int32 i = 0; i < DiscoveredRigs.Num(); i++)
			{
				UCameraComponent* Cam = DiscoveredRigs[i].Camera.Get();
				if (Cam && Cam->GetOwner() == ViewTargetActor)
				{
					TargetIdx = i;
					break;
				}
			}
		}
		if (TargetIdx >= 0 && TargetIdx != ActiveRigIndex)
		{
			ActiveRigIndex = TargetIdx;
			UE_LOG(LogDisplayXRPreviewSession, Log,
				TEXT("DisplayXR Preview: Auto-follow (view target) → %s"), *GetActiveRigName());
		}
	}

	// Render scene captures and blit to swapchain
	if (LeftCapture && RightCapture && SwapchainImages.Num() > 0)
	{
		RenderAndBlit(ImageIndex);
	}

	XrSwapchainImageReleaseInfo RelInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	xrReleaseSwapchainImageFunc(Swapchain, &RelInfo);

	// Locate views for eye positions (used in projection layer for runtime weaving)
	XrView EyeViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t LocatedViewCount = 0;
	{
		XrViewLocateInfo LocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
		LocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		LocateInfo.displayTime = PredictedDisplayTime;
		LocateInfo.space = ViewSpace;
		XrViewState ViewState = {XR_TYPE_VIEW_STATE};
		xrLocateViewsFunc(Session, &LocateInfo, &ViewState, 2, &LocatedViewCount, EyeViews);
	}

	// Build projection layer (same pattern as DisplayXRCompositor::CompositorLoop)
	int32 NV = ViewConfig.GetViewCount();
	int32 TW = ViewConfig.GetTileW();
	int32 TH = ViewConfig.GetTileH();
	int32 Cols = FMath::Max(ViewConfig.TileColumns, 1);

	TArray<XrCompositionLayerProjectionView> ProjViews;
	ProjViews.SetNum(NV);
	for (int32 i = 0; i < NV; i++)
	{
		ProjViews[i] = {};
		ProjViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		// Use real eye positions from xrLocateViews if available
		if (i < (int32)LocatedViewCount)
		{
			ProjViews[i].pose = EyeViews[i].pose;
			ProjViews[i].fov = EyeViews[i].fov;
		}
		else
		{
			ProjViews[i].pose.orientation = {0, 0, 0, 1};
			ProjViews[i].pose.position = {0, 0, 0};
			ProjViews[i].fov = {-0.5f, 0.5f, 0.3f, -0.3f};
		}
		ProjViews[i].subImage.swapchain = Swapchain;
		ProjViews[i].subImage.imageRect.offset = {(i % Cols) * TW, (i / Cols) * TH};
		ProjViews[i].subImage.imageRect.extent = {TW, TH};
	}

	XrCompositionLayerProjection ProjLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	ProjLayer.space = ViewSpace;
	ProjLayer.viewCount = (uint32_t)NV;
	ProjLayer.views = ProjViews.GetData();
	const XrCompositionLayerBaseHeader* Layers[] = {(const XrCompositionLayerBaseHeader*)&ProjLayer};

	XrFrameEndInfo EndInfo = {XR_TYPE_FRAME_END_INFO};
	EndInfo.displayTime = PredictedDisplayTime;
	EndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	EndInfo.layerCount = 1;
	EndInfo.layers = Layers;
	xrEndFrameFunc(Session, &EndInfo);

	FrameCount++;
	if (FrameCount <= 3 || FrameCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Frame %d submitted"), FrameCount);
	}
}

// =============================================================================
// OpenXR runtime loading (copied from DisplayXRSession.cpp pattern)
// =============================================================================

bool FDisplayXRPreviewSession::LoadOpenXRRuntime()
{
#if PLATFORM_WINDOWS
	// Negotiate function types
	struct XrNegotiateLoaderInfo
	{
		uint32_t structType;
		uint32_t structVersion;
		size_t structSize;
		uint32_t minInterfaceVersion;
		uint32_t maxInterfaceVersion;
		uint64_t minApiVersion;
		uint64_t maxApiVersion;
	};
	struct XrNegotiateRuntimeRequest
	{
		uint32_t structType;
		uint32_t structVersion;
		size_t structSize;
		uint32_t runtimeInterfaceVersion;
		uint64_t runtimeApiVersion;
		PFN_xrGetInstanceProcAddr getInstanceProcAddr;
	};
	typedef XrResult(XRAPI_PTR* PFN_xrNegotiateLoaderRuntimeInterface)(
		const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest);

	// Step 1: Find runtime DLL path from manifest JSON
	FString RuntimeDllPath;
	{
		FString RuntimeJsonPath = FPlatformMisc::GetEnvironmentVariable(TEXT("XR_RUNTIME_JSON"));

		if (RuntimeJsonPath.IsEmpty())
		{
			HKEY hKey = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1",
				0, KEY_READ, &hKey) == ERROR_SUCCESS)
			{
				WCHAR Buffer[1024] = {};
				DWORD BufferSize = sizeof(Buffer) - sizeof(WCHAR);
				DWORD Type = 0;
				if (RegQueryValueExW(hKey, L"ActiveRuntime", nullptr, &Type, (LPBYTE)Buffer, &BufferSize) == ERROR_SUCCESS
					&& Type == REG_SZ)
				{
					RuntimeJsonPath = FString(Buffer);
				}
				RegCloseKey(hKey);
			}
		}

		if (!RuntimeJsonPath.IsEmpty())
		{
			UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Runtime JSON: %s"), *RuntimeJsonPath);

			FString JsonContent;
			if (FFileHelper::LoadFileToString(JsonContent, *RuntimeJsonPath))
			{
				TSharedPtr<FJsonObject> JsonRoot;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
				if (FJsonSerializer::Deserialize(Reader, JsonRoot) && JsonRoot.IsValid())
				{
					const TSharedPtr<FJsonObject>* RuntimeObj;
					if (JsonRoot->TryGetObjectField(TEXT("runtime"), RuntimeObj))
					{
						FString LibPath;
						if ((*RuntimeObj)->TryGetStringField(TEXT("library_path"), LibPath))
						{
							if (FPaths::IsRelative(LibPath))
							{
								RuntimeDllPath = FPaths::GetPath(RuntimeJsonPath) / LibPath;
							}
							else
							{
								RuntimeDllPath = LibPath;
							}
							FPaths::NormalizeFilename(RuntimeDllPath);
						}
					}
				}
			}
		}
	}

	// Step 2: Try loading runtime DLL and negotiating
	if (!RuntimeDllPath.IsEmpty())
	{
		LoaderHandle = (void*)LoadLibraryW(*RuntimeDllPath);
		if (LoaderHandle)
		{
			UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Loaded runtime DLL: %s"), *RuntimeDllPath);

			auto NegotiateFunc = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
				reinterpret_cast<void*>(GetProcAddress((HMODULE)LoaderHandle, "xrNegotiateLoaderRuntimeInterface")));

			if (NegotiateFunc)
			{
				XrNegotiateLoaderInfo LoaderInfo = {};
				LoaderInfo.structType = 1;
				LoaderInfo.structVersion = 1;
				LoaderInfo.structSize = sizeof(LoaderInfo);
				LoaderInfo.minInterfaceVersion = 1;
				LoaderInfo.maxInterfaceVersion = 1;
				LoaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
				LoaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

				XrNegotiateRuntimeRequest RuntimeRequest = {};
				RuntimeRequest.structType = 3;
				RuntimeRequest.structVersion = 1;
				RuntimeRequest.structSize = sizeof(RuntimeRequest);

				XrResult Result = NegotiateFunc(&LoaderInfo, &RuntimeRequest);
				if (XR_SUCCEEDED(Result) && RuntimeRequest.getInstanceProcAddr)
				{
					xrGetInstanceProcAddrFunc = RuntimeRequest.getInstanceProcAddr;
					UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Got xrGetInstanceProcAddr via negotiate"));
					return true;
				}
				else
				{
					UE_LOG(LogDisplayXRPreviewSession, Warning, TEXT("DisplayXR Preview: Negotiate failed (%d), trying direct export"), (int)Result);
				}
			}

			// Fallback: direct xrGetInstanceProcAddr export
			xrGetInstanceProcAddrFunc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
				reinterpret_cast<void*>(GetProcAddress((HMODULE)LoaderHandle, "xrGetInstanceProcAddr")));
			if (xrGetInstanceProcAddrFunc)
			{
				UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Got xrGetInstanceProcAddr directly from runtime DLL"));
				return true;
			}

			FreeLibrary((HMODULE)LoaderHandle);
			LoaderHandle = nullptr;
		}
		else
		{
			UE_LOG(LogDisplayXRPreviewSession, Warning, TEXT("DisplayXR Preview: Failed to load runtime DLL: %s"), *RuntimeDllPath);
		}
	}

	// Step 3: Fallback — try loader DLLs
	const TCHAR* FallbackPaths[] = {
		TEXT("displayxr_openxr.dll"),
		TEXT("openxr_loader.dll"),
		nullptr
	};

	for (int i = 0; FallbackPaths[i] != nullptr; i++)
	{
		LoaderHandle = (void*)LoadLibraryW(FallbackPaths[i]);
		if (LoaderHandle)
		{
			UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Loaded fallback: %s"), FallbackPaths[i]);
			break;
		}
	}

	if (!LoaderHandle)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to load any OpenXR runtime or loader"));
		return false;
	}

	xrGetInstanceProcAddrFunc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
		reinterpret_cast<void*>(GetProcAddress((HMODULE)LoaderHandle, "xrGetInstanceProcAddr")));

	if (!xrGetInstanceProcAddrFunc)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: No xrGetInstanceProcAddr in loader"));
		UnloadOpenXRRuntime();
		return false;
	}

	return true;
#else
	UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Editor preview only supported on Windows"));
	return false;
#endif
}

void FDisplayXRPreviewSession::UnloadOpenXRRuntime()
{
	if (LoaderHandle)
	{
#if PLATFORM_WINDOWS
		FreeLibrary((HMODULE)LoaderHandle);
#endif
		LoaderHandle = nullptr;
	}
	xrGetInstanceProcAddrFunc = nullptr;
}

// =============================================================================
// Instance creation
// =============================================================================

bool FDisplayXRPreviewSession::CreateXrInstance()
{
	PFN_xrCreateInstance xrCreateInstanceFunc = nullptr;
	xrGetInstanceProcAddrFunc(XR_NULL_HANDLE, "xrCreateInstance",
		(PFN_xrVoidFunction*)&xrCreateInstanceFunc);
	if (!xrCreateInstanceFunc)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: No xrCreateInstance"));
		return false;
	}

	const char* Extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
#if PLATFORM_WINDOWS
		"XR_KHR_D3D12_enable",
		XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME,
#endif
	};

	XrInstanceCreateInfo CreateInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	FCStringAnsi::Strncpy(CreateInfo.applicationInfo.applicationName, "DisplayXR Editor Preview", XR_MAX_APPLICATION_NAME_SIZE);
	CreateInfo.applicationInfo.applicationVersion = 1;
	FCStringAnsi::Strncpy(CreateInfo.applicationInfo.engineName, "Unreal Engine", XR_MAX_ENGINE_NAME_SIZE);
	CreateInfo.applicationInfo.engineVersion = 5;
	CreateInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	CreateInfo.enabledExtensionCount = UE_ARRAY_COUNT(Extensions);
	CreateInfo.enabledExtensionNames = Extensions;

	XrResult Result = xrCreateInstanceFunc(&CreateInfo, &Instance);
	if (!XR_SUCCEEDED(Result))
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: xrCreateInstance failed (%d)"), (int)Result);
		return false;
	}

	// Get system
	PFN_xrGetSystem xrGetSystemFunc = nullptr;
	xrGetInstanceProcAddrFunc(Instance, "xrGetSystem", (PFN_xrVoidFunction*)&xrGetSystemFunc);

	XrSystemGetInfo SystemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
	SystemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	Result = xrGetSystemFunc(Instance, &SystemGetInfo, &SystemId);
	if (!XR_SUCCEEDED(Result))
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: xrGetSystem failed (%d)"), (int)Result);
		return false;
	}

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Instance created, SystemId=%llu"), (unsigned long long)SystemId);
	return true;
}

void FDisplayXRPreviewSession::DestroyXrInstance()
{
	if (Instance != XR_NULL_HANDLE)
	{
		PFN_xrDestroyInstance xrDestroyInstanceFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&xrDestroyInstanceFunc);
		if (xrDestroyInstanceFunc)
		{
			xrDestroyInstanceFunc(Instance);
		}
		Instance = XR_NULL_HANDLE;
		SystemId = XR_NULL_SYSTEM_ID;
	}
}

// =============================================================================
// Display info + rendering modes
// =============================================================================

void FDisplayXRPreviewSession::QueryDisplayInfo()
{
	PFN_xrGetSystemProperties xrGetSystemPropertiesFunc = nullptr;
	xrGetInstanceProcAddrFunc(Instance, "xrGetSystemProperties",
		(PFN_xrVoidFunction*)&xrGetSystemPropertiesFunc);
	if (!xrGetSystemPropertiesFunc)
	{
		return;
	}

	XrSystemProperties SystemProps = {XR_TYPE_SYSTEM_PROPERTIES};
	XrDisplayInfoEXT DisplayInfoExt = {};
	DisplayInfoExt.type = (XrStructureType)XR_TYPE_DISPLAY_INFO_EXT;
	SystemProps.next = &DisplayInfoExt;

	XrResult Result = xrGetSystemPropertiesFunc(Instance, SystemId, &SystemProps);
	if (XR_SUCCEEDED(Result))
	{
		DisplayInfo.DisplayWidthMeters = DisplayInfoExt.displaySizeMeters.width;
		DisplayInfo.DisplayHeightMeters = DisplayInfoExt.displaySizeMeters.height;
		DisplayInfo.DisplayPixelWidth = (int32)DisplayInfoExt.displayPixelWidth;
		DisplayInfo.DisplayPixelHeight = (int32)DisplayInfoExt.displayPixelHeight;
		DisplayInfo.NominalViewerPosition = FVector(
			DisplayInfoExt.nominalViewerPositionInDisplaySpace.x,
			DisplayInfoExt.nominalViewerPositionInDisplaySpace.y,
			DisplayInfoExt.nominalViewerPositionInDisplaySpace.z);
		DisplayInfo.RecommendedViewScaleX = DisplayInfoExt.recommendedViewScaleX;
		DisplayInfo.RecommendedViewScaleY = DisplayInfoExt.recommendedViewScaleY;
		DisplayInfo.bIsValid = true;

		UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Display %.3f x %.3f m, %d x %d px"),
			DisplayInfo.DisplayWidthMeters, DisplayInfo.DisplayHeightMeters,
			DisplayInfo.DisplayPixelWidth, DisplayInfo.DisplayPixelHeight);
	}
}

void FDisplayXRPreviewSession::QueryRenderingModes()
{
	if (!xrEnumerateDisplayRenderingModesFunc || Session == XR_NULL_HANDLE)
	{
		UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: No rendering mode enumeration available"));
		return;
	}

	uint32_t ModeCount = 0;
	XrResult Result = xrEnumerateDisplayRenderingModesFunc(Session, 0, &ModeCount, nullptr);
	if (!XR_SUCCEEDED(Result) || ModeCount == 0)
	{
		UE_LOG(LogDisplayXRPreviewSession, Warning, TEXT("DisplayXR Preview: No rendering modes found (%d)"), (int)Result);
		return;
	}

	TArray<XrDisplayRenderingModeInfoEXT> Modes;
	Modes.SetNum(ModeCount);
	for (uint32_t i = 0; i < ModeCount; i++)
	{
		Modes[i].type = (XrStructureType)XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		Modes[i].next = nullptr;
	}

	Result = xrEnumerateDisplayRenderingModesFunc(Session, ModeCount, &ModeCount, Modes.GetData());
	if (!XR_SUCCEEDED(Result))
	{
		return;
	}

	for (uint32_t i = 0; i < ModeCount; i++)
	{
		const auto& M = Modes[i];
		UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Mode[%d] '%s' views=%d scale=%.2fx%.2f tiles=%dx%d viewPx=%dx%d hw3D=%d"),
			M.modeIndex, ANSI_TO_TCHAR(M.modeName), M.viewCount,
			M.viewScaleX, M.viewScaleY, M.tileColumns, M.tileRows,
			M.viewWidthPixels, M.viewHeightPixels, (int)M.hardwareDisplay3D);

		// Use the 3D mode to populate ViewConfig
		if (M.hardwareDisplay3D && M.viewCount >= 2)
		{
			ViewConfig.TileColumns = (int32)M.tileColumns;
			ViewConfig.TileRows = (int32)M.tileRows;
			ViewConfig.ScaleX = M.viewScaleX;
			ViewConfig.ScaleY = M.viewScaleY;
			if (M.viewWidthPixels > 0 && M.viewHeightPixels > 0 && M.viewScaleX > 0.0f && M.viewScaleY > 0.0f)
			{
				ViewConfig.DisplayPixelW = (int32)(M.viewWidthPixels / M.viewScaleX);
				ViewConfig.DisplayPixelH = (int32)(M.viewHeightPixels / M.viewScaleY);
			}

			UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Using 3D mode — %dx%d tiles, scale %.2fx%.2f, display %dx%d px"),
				ViewConfig.TileColumns, ViewConfig.TileRows, ViewConfig.ScaleX, ViewConfig.ScaleY,
				ViewConfig.DisplayPixelW, ViewConfig.DisplayPixelH);
		}
	}
}

// =============================================================================
// D3D12 runtime queue
// =============================================================================

bool FDisplayXRPreviewSession::CreateRuntimeQueue()
{
#if PLATFORM_WINDOWS
	ID3D12Device* UEDev = static_cast<ID3D12Device*>(D3DDevice);
	if (!UEDev) return false;

	D3D12_COMMAND_QUEUE_DESC QD = {};
	QD.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	QD.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	QD.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	QD.NodeMask = 0;

	HRESULT hr = UEDev->CreateCommandQueue(&QD,
		IID_PPV_ARGS(reinterpret_cast<ID3D12CommandQueue**>(&RuntimeQueue)));
	if (FAILED(hr) || !RuntimeQueue)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: RuntimeQueue creation failed (0x%08x)"), (unsigned)hr);
		return false;
	}

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: RuntimeQueue created on UE device (%p)"), RuntimeQueue);
	return true;
#else
	return false;
#endif
}

// =============================================================================
// Native preview window (standalone, like Unity)
// =============================================================================

bool FDisplayXRPreviewSession::CreateNativeWindow()
{
#if PLATFORM_WINDOWS
	if (!RegPreviewWindowClass()) return false;

	int W = DisplayInfo.DisplayPixelWidth > 0 ? DisplayInfo.DisplayPixelWidth : 3840;
	int H = DisplayInfo.DisplayPixelHeight > 0 ? DisplayInfo.DisplayPixelHeight : 2160;

	GPreviewSessionForWndProc = this;

	// Capture the foreground window (editor/PIE viewport) so we can restore
	// focus to it after showing the preview, and so the WndProc can forward
	// any stray keyboard events back to PIE.
	HWND PriorForeground = GetForegroundWindow();
	GPreviewEditorHWND = PriorForeground;

	// WS_EX_NOACTIVATE + MA_NOACTIVATE: clicking the preview doesn't steal
	// focus or input from the PIE viewport.
	// WS_EX_TOPMOST: preview stays visible when the user clicks into the PIE
	// viewport (which brings the editor window to foreground). Without
	// TOPMOST, the preview would disappear behind the editor as soon as PIE
	// captures input.
	PreviewHWND = CreateWindowExW(
		WS_EX_NOACTIVATE | WS_EX_TOPMOST,
		L"DisplayXRPreview", L"DisplayXR Preview",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, W, H,
		NULL, NULL, GetModuleHandleW(0), NULL);

	if (!PreviewHWND)
	{
		GPreviewSessionForWndProc = nullptr;
		return false;
	}

	ShowWindow((HWND)PreviewHWND, SW_SHOWNOACTIVATE);

	// Return focus to the editor so PIE viewport receives keyboard/mouse input.
	if (PriorForeground && PriorForeground != (HWND)PreviewHWND)
	{
		SetForegroundWindow(PriorForeground);
	}
	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Native window created %dx%d"), W, H);
	return true;
#else
	return false;
#endif
}

void FDisplayXRPreviewSession::DestroyNativeWindow()
{
#if PLATFORM_WINDOWS
	GPreviewSessionForWndProc = nullptr;
	if (PreviewHWND)
	{
		DestroyWindow((HWND)PreviewHWND);
		PreviewHWND = nullptr;
	}
#endif
}

void FDisplayXRPreviewSession::UpdateCanvasRect()
{
#if PLATFORM_WINDOWS
	if (!PreviewHWND || !xrSetOutputRectFunc || Session == XR_NULL_HANDLE) return;

	RECT rc;
	if (GetClientRect((HWND)PreviewHWND, &rc))
	{
		uint32 W = (uint32)(rc.right - rc.left);
		uint32 H = (uint32)(rc.bottom - rc.top);
		if (W > 0 && H > 0)
		{
			static uint32 LastW = 0, LastH = 0;
			if (W != LastW || H != LastH)
			{
				xrSetOutputRectFunc(Session, 0, 0, W, H);
				UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Canvas rect updated: %dx%d"), W, H);
				LastW = W;
				LastH = H;
			}
		}
	}
#endif
}

void FDisplayXRPreviewSession::OnNativeWindowClosed()
{
	bWindowClosed = true;
}

// =============================================================================
// Session creation with D3D12 + Win32 bindings
// =============================================================================

bool FDisplayXRPreviewSession::CreateXrSession()
{
	if (Instance == XR_NULL_HANDLE || !xrGetInstanceProcAddrFunc)
	{
		return false;
	}

	PFN_xrCreateSession xrCreateSessionFunc = nullptr;
	xrGetInstanceProcAddrFunc(Instance, "xrCreateSession",
		(PFN_xrVoidFunction*)&xrCreateSessionFunc);
	if (!xrCreateSessionFunc)
	{
		return false;
	}

#if PLATFORM_WINDOWS
	// D3D12 graphics binding
	struct XrGraphicsBindingD3D12KHR
	{
		XrStructureType type;
		const void* next;
		void* device;
		void* queue;
	};

	XrGraphicsBindingD3D12KHR D3D12Binding = {};
	D3D12Binding.type = (XrStructureType)XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
	D3D12Binding.device = D3DDevice;
	D3D12Binding.queue = RuntimeQueue;

	// Chain Win32 window binding — use the native preview window
	XrWin32WindowBindingCreateInfoEXT Win32Binding = {};
	Win32Binding.type = (XrStructureType)XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	Win32Binding.windowHandle = PreviewHWND;

	if (PreviewHWND)
	{
		D3D12Binding.next = &Win32Binding;
	}

	// Call xrGetD3D12GraphicsRequirementsKHR (required before xrCreateSession)
	{
		typedef struct XrGraphicsRequirementsD3D12KHR
		{
			XrStructureType type;
			void* next;
			int64_t adapterLuid;
			int32_t minFeatureLevel;
		} XrGraphicsRequirementsD3D12KHR;

		typedef XrResult(XRAPI_PTR* PFN_xrGetD3D12GraphicsRequirementsKHR)(
			XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* graphicsRequirements);

		PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrGetD3D12GraphicsRequirementsKHR",
			(PFN_xrVoidFunction*)&xrGetD3D12GraphicsRequirementsFunc);

		if (xrGetD3D12GraphicsRequirementsFunc)
		{
			XrGraphicsRequirementsD3D12KHR Reqs = {};
			Reqs.type = (XrStructureType)XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
			xrGetD3D12GraphicsRequirementsFunc(Instance, SystemId, &Reqs);
			UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: D3D12 graphics requirements queried"));
		}
	}

	XrSessionCreateInfo SessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
	SessionCreateInfo.systemId = SystemId;
	SessionCreateInfo.next = &D3D12Binding;

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Creating session device=%p queue=%p hwnd=%p"),
		D3DDevice, RuntimeQueue, PreviewHWND);

	XrResult Result = xrCreateSessionFunc(Instance, &SessionCreateInfo, &Session);
	if (!XR_SUCCEEDED(Result))
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: xrCreateSession failed (%d)"), (int)Result);
		return false;
	}
#else
	return false;
#endif

	// Create LOCAL reference space
	PFN_xrCreateReferenceSpace xrCreateReferenceSpaceFunc = nullptr;
	xrGetInstanceProcAddrFunc(Instance, "xrCreateReferenceSpace",
		(PFN_xrVoidFunction*)&xrCreateReferenceSpaceFunc);
	if (xrCreateReferenceSpaceFunc)
	{
		XrReferenceSpaceCreateInfo SpaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		SpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		SpaceCreateInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
		SpaceCreateInfo.poseInReferenceSpace.position = {0, 0, 0};
		xrCreateReferenceSpaceFunc(Session, &SpaceCreateInfo, &ViewSpace);
	}

	// Resolve extension function pointers
	xrGetInstanceProcAddrFunc(Instance, "xrRequestDisplayModeEXT",
		(PFN_xrVoidFunction*)&xrRequestDisplayModeFunc);
	xrGetInstanceProcAddrFunc(Instance, "xrEnumerateDisplayRenderingModesEXT",
		(PFN_xrVoidFunction*)&xrEnumerateDisplayRenderingModesFunc);
	xrGetInstanceProcAddrFunc(Instance, "xrSetSharedTextureOutputRectEXT",
		(PFN_xrVoidFunction*)&xrSetOutputRectFunc);

	// Push initial canvas rect
	UpdateCanvasRect();

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Session created (outputRect=%s)"),
		xrSetOutputRectFunc ? TEXT("yes") : TEXT("no"));
	return true;
}

void FDisplayXRPreviewSession::DestroyXrSession()
{
	if (ViewSpace != XR_NULL_HANDLE && xrGetInstanceProcAddrFunc && Instance != XR_NULL_HANDLE)
	{
		PFN_xrDestroySpace xrDestroySpaceFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrDestroySpace",
			(PFN_xrVoidFunction*)&xrDestroySpaceFunc);
		if (xrDestroySpaceFunc)
		{
			xrDestroySpaceFunc(ViewSpace);
		}
		ViewSpace = XR_NULL_HANDLE;
	}

	if (Session != XR_NULL_HANDLE && xrGetInstanceProcAddrFunc && Instance != XR_NULL_HANDLE)
	{
		PFN_xrDestroySession xrDestroySessionFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrDestroySession",
			(PFN_xrVoidFunction*)&xrDestroySessionFunc);
		if (xrDestroySessionFunc)
		{
			xrDestroySessionFunc(Session);
		}
		Session = XR_NULL_HANDLE;
	}
}

// =============================================================================
// Resolve XR function pointers
// =============================================================================

bool FDisplayXRPreviewSession::ResolveXrFunctions()
{
	if (!xrGetInstanceProcAddrFunc || Instance == XR_NULL_HANDLE) return false;

	auto R = [&](const char* N, PFN_xrVoidFunction* F) {
		return XR_SUCCEEDED(xrGetInstanceProcAddrFunc(Instance, N, F)) && *F;
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
	ok &= R("xrLocateViews", (PFN_xrVoidFunction*)&xrLocateViewsFunc);
	return ok;
}

// =============================================================================
// Swapchain
// =============================================================================

bool FDisplayXRPreviewSession::CreateSwapchain()
{
	if (Session == XR_NULL_HANDLE || !xrCreateSwapchainFunc) return false;

	// Swapchain at FULL display resolution — tiles are sub-images within it
	SwapchainWidth = DisplayInfo.DisplayPixelWidth > 0 ? (uint32)DisplayInfo.DisplayPixelWidth : 3840;
	SwapchainHeight = DisplayInfo.DisplayPixelHeight > 0 ? (uint32)DisplayInfo.DisplayPixelHeight : 2160;
	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Swapchain target: %dx%d"), SwapchainWidth, SwapchainHeight);

	if (!SwapchainWidth || !SwapchainHeight) return false;

	// Enumerate supported formats
	uint32_t FC = 0;
	xrEnumerateSwapchainFormatsFunc(Session, 0, &FC, nullptr);
	if (!FC) return false;
	TArray<int64_t> Fmts; Fmts.SetNum(FC);
	xrEnumerateSwapchainFormatsFunc(Session, FC, &FC, Fmts.GetData());

	// Prefer B8G8R8A8 (87) or R8G8B8A8 (28)
	SwapchainFormat = Fmts[0];
	for (auto F : Fmts) { if (F == 87) { SwapchainFormat = F; break; } if (F == 28) SwapchainFormat = F; }

	XrSwapchainCreateInfo CI = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	CI.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	CI.format = SwapchainFormat;
	CI.sampleCount = 1;
	CI.width = SwapchainWidth;
	CI.height = SwapchainHeight;
	CI.faceCount = 1;
	CI.arraySize = 1;
	CI.mipCount = 1;

	XrResult r = xrCreateSwapchainFunc(Session, &CI, &Swapchain);
	if (!XR_SUCCEEDED(r))
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: xrCreateSwapchain failed (%d)"), (int)r);
		return false;
	}

	// Enumerate swapchain images
	uint32_t IC = 0;
	xrEnumerateSwapchainImagesFunc(Swapchain, 0, &IC, nullptr);
	TArray<XrSwapchainImageD3D12KHR> Imgs; Imgs.SetNum(IC);
	for (uint32_t i = 0; i < IC; i++)
	{
		Imgs[i] = {};
		Imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR_VALUE;
	}
	xrEnumerateSwapchainImagesFunc(Swapchain, IC, &IC, (XrSwapchainImageBaseHeader*)Imgs.GetData());

	SwapchainImages.SetNum(IC);
	for (uint32_t i = 0; i < IC; i++)
	{
		SwapchainImages[i].D3D12Resource = Imgs[i].texture;
	}

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Swapchain %dx%d fmt=%lld (%d images)"),
		SwapchainWidth, SwapchainHeight, (long long)SwapchainFormat, IC);
	return true;
}

void FDisplayXRPreviewSession::DestroySwapchain()
{
	if (Swapchain != XR_NULL_HANDLE && xrDestroySwapchainFunc)
	{
		xrDestroySwapchainFunc(Swapchain);
		Swapchain = XR_NULL_HANDLE;
	}
	SwapchainImages.Empty();
}

// =============================================================================
// Wrap swapchain images as RHI textures (for GPU copy)
// =============================================================================

bool FDisplayXRPreviewSession::WrapSwapchainImagesAsRHI()
{
#if PLATFORM_WINDOWS
	if (SwapchainImages.Num() == 0) return false;

	SwapchainImagesRHI.Empty(SwapchainImages.Num());

	ID3D12DynamicRHI* DynamicRHI = GetID3D12DynamicRHI();
	if (!DynamicRHI)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: GetID3D12DynamicRHI returned null"));
		return false;
	}

	const ETextureCreateFlags Flags =
		ETextureCreateFlags::RenderTargetable |
		ETextureCreateFlags::ShaderResource |
		ETextureCreateFlags::ResolveTargetable |
		ETextureCreateFlags::Dynamic;

	TArray<FTextureRHIRef>& OutImages = SwapchainImagesRHI;
	TArray<FSwapchainImage> InImages = SwapchainImages;
	int64 Fmt = SwapchainFormat;
	ENQUEUE_RENDER_COMMAND(WrapDisplayXRPreviewSwapchainImages)(
		[&OutImages, InImages, DynamicRHI, Flags, Fmt](FRHICommandListImmediate& RHICmdList)
		{
			for (const FSwapchainImage& Img : InImages)
			{
				ID3D12Resource* Res = static_cast<ID3D12Resource*>(Img.D3D12Resource);
				// Match the pixel format to the actual swapchain DXGI format.
				// Format 28 = R8G8B8A8_UNORM → PF_R8G8B8A8
				// Format 87 = B8G8R8A8_UNORM → PF_B8G8R8A8
				EPixelFormat WrapFmt = (Fmt == 28) ? PF_R8G8B8A8 : PF_B8G8R8A8;
				FTextureRHIRef Wrapped = DynamicRHI->RHICreateTexture2DFromResource(
					WrapFmt, Flags, FClearValueBinding::Transparent, Res);
				OutImages.Add(Wrapped);
			}
		});
	FlushRenderingCommands();

	bSwapchainWrapped = SwapchainImagesRHI.Num() > 0;
	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Wrapped %d swapchain images as RHI"), SwapchainImagesRHI.Num());
	return bSwapchainWrapped;
#else
	return false;
#endif
}

// =============================================================================
// Scene capture creation / destruction
// =============================================================================

bool FDisplayXRPreviewSession::CreateSceneCaptures()
{
	UWorld* World = GetPreviewTargetWorld();
	if (!World)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: No PIE world available"));
		return false;
	}

	const int32 TileW = ViewConfig.GetTileW();
	const int32 TileH = ViewConfig.GetTileH();

	// Create render targets at tile resolution.
	// Use RTF_RGBA8_SRGB → PF_B8G8R8A8 with SRGB=true so the RTV is _UNORM_SRGB.
	// Hardware applies the sRGB OETF on write, matching what game mode stores
	// in its swapchain-wrapped RT. A raw CopyTextureRegion to the B8G8R8A8_UNORM
	// swapchain then delivers bytes the DisplayXR runtime composites correctly.
	LeftRenderTarget = NewObject<UTextureRenderTarget2D>();
	LeftRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8_SRGB;
	LeftRenderTarget->InitAutoFormat(TileW, TileH);
	LeftRenderTarget->UpdateResourceImmediate(true);

	RightRenderTarget = NewObject<UTextureRenderTarget2D>();
	RightRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8_SRGB;
	RightRenderTarget->InitAutoFormat(TileW, TileH);
	RightRenderTarget->UpdateResourceImmediate(true);

	// Spawn a transient actor to host the capture components
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CaptureActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (!CaptureActor)
	{
		UE_LOG(LogDisplayXRPreviewSession, Error, TEXT("DisplayXR Preview: Failed to spawn capture actor"));
		return false;
	}

	// Helper to configure a capture component.
	// Match the main viewport's rendering pipeline as closely as possible:
	//  - bAlwaysPersistRenderingState keeps TAA history + eye-adaptation state
	//    across manual CaptureScene() calls, matching how the viewport renders.
	//  - MotionBlur off: single-frame captures have no usable velocity history.
	//  - Remaining show flags inherit SceneCapture defaults, which include
	//    tonemap, color grading, bloom, SSR, AA, etc.
	//  - PostProcessSettings left at defaults with no bOverride_* flags, so
	//    world post-process volumes apply normally.
	auto ConfigureCapture = [](USceneCaptureComponent2D* Capture, UTextureRenderTarget2D* Target)
	{
		Capture->TextureTarget = Target;
		Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bUseCustomProjectionMatrix = true;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->ShowFlags.SetMotionBlur(false);
		Capture->RegisterComponent();
	};

	LeftCapture = NewObject<USceneCaptureComponent2D>(CaptureActor);
	ConfigureCapture(LeftCapture, LeftRenderTarget);

	RightCapture = NewObject<USceneCaptureComponent2D>(CaptureActor);
	ConfigureCapture(RightCapture, RightRenderTarget);

	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Scene captures created at %dx%d"), TileW, TileH);
	return true;
}

void FDisplayXRPreviewSession::DestroySceneCaptures()
{
	if (LeftCapture) { LeftCapture->DestroyComponent(); LeftCapture = nullptr; }
	if (RightCapture) { RightCapture->DestroyComponent(); RightCapture = nullptr; }
	if (CaptureActor) { CaptureActor->Destroy(); CaptureActor = nullptr; }
	LeftRenderTarget = nullptr;
	RightRenderTarget = nullptr;
}

// =============================================================================
// Rig discovery + selection
// =============================================================================

void FDisplayXRPreviewSession::ScanForRigs()
{
	DiscoveredRigs.Empty();

	UWorld* World = GetPreviewTargetWorld();
	if (!World) return;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		UDisplayXRCamera* CameraRig = Actor->FindComponentByClass<UDisplayXRCamera>();
		UDisplayXRDisplay* DisplayRig = Actor->FindComponentByClass<UDisplayXRDisplay>();

		if (!CameraRig && !DisplayRig) continue;

		UCameraComponent* Camera = Actor->FindComponentByClass<UCameraComponent>();
		if (!Camera) continue;

		FRigEntry Entry;
		Entry.Camera = Camera;
		Entry.CameraRig = CameraRig;
		Entry.DisplayRig = DisplayRig;
		DiscoveredRigs.Add(Entry);
	}

	if (DiscoveredRigs.Num() > 0)
	{
		// Prefer the player's current view target (what game mode actually
		// renders from). Falls back to the locally-controlled pawn, then to
		// rig index 0 if neither is set yet.
		ActiveRigIndex = 0;
		AActor* ViewTargetActor = nullptr;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* PC = It->Get())
			{
				if (AActor* VT = PC->GetViewTarget()) { ViewTargetActor = VT; break; }
			}
		}
		for (int32 i = 0; i < DiscoveredRigs.Num(); i++)
		{
			UCameraComponent* Cam = DiscoveredRigs[i].Camera.Get();
			if (!Cam) continue;
			if (ViewTargetActor && Cam->GetOwner() == ViewTargetActor)
			{
				ActiveRigIndex = i;
				break;
			}
			if (APawn* OwnerPawn = Cast<APawn>(Cam->GetOwner()))
			{
				if (OwnerPawn->IsLocallyControlled())
				{
					ActiveRigIndex = i; // tentative — keep searching for a view-target match
				}
			}
		}
		UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Found %d rig(s), active: %s"),
			DiscoveredRigs.Num(), *GetActiveRigName());
	}
	else
	{
		ActiveRigIndex = -1;
		UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: No rigs found, using editor viewport camera"));
	}
}

void FDisplayXRPreviewSession::FocusPIEViewport()
{
	// Bring the editor to foreground so PIE can own input.
	if (GPreviewEditorHWND && IsWindow(GPreviewEditorHWND))
	{
		SetForegroundWindow(GPreviewEditorHWND);
	}

	// Give the PIE viewport Slate keyboard focus so WASD reaches the pawn.
	if (!GEditor || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FViewport* PIEViewport = GEditor->GetPIEViewport();
	if (!PIEViewport)
	{
		return;
	}
	FSceneViewport* SceneVp = static_cast<FSceneViewport*>(PIEViewport);
	TSharedPtr<SViewport> ViewportWidget = SceneVp->GetViewportWidget().Pin();
	if (ViewportWidget.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
		FSlateApplication::Get().SetUserFocus(0, ViewportWidget, EFocusCause::SetDirectly);
	}
}

void FDisplayXRPreviewSession::ForwardMouseLookDelta(int DeltaX, int DeltaY)
{
	// Feed raw pixel delta into the player controller's rotation input —
	// same accumulator UE's own mouse-look uses (via UPlayerInput). Avoids
	// OS cursor warping and SendInput synchronization pitfalls. Sensitivity
	// matches UE's default axis scale (roughly 2.5 deg/sec per pixel of
	// mouse X at 60fps, empirically).
	UWorld* World = GetPreviewTargetWorld();
	if (!World)
	{
		return;
	}
	APlayerController* PC = nullptr;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* Candidate = It->Get())
		{
			PC = Candidate;
			break;
		}
	}
	if (!PC)
	{
		return;
	}

	// Default UE "Turn" / "LookUp" scale is 45 deg per mouse unit; typical
	// project input maps have a smaller scale in the axis mapping itself.
	// 0.1 gives a feel comparable to holding-RMB-drag in editor viewport.
	const float Sensitivity = 0.1f;
	PC->AddYawInput((float)DeltaX * Sensitivity);
	PC->AddPitchInput((float)DeltaY * Sensitivity);
}

void FDisplayXRPreviewSession::CycleRig()
{
	// Rescan in case rigs were added/removed
	ScanForRigs();

	if (DiscoveredRigs.Num() == 0)
	{
		ActiveRigIndex = -1;
		return;
	}

	ActiveRigIndex = (ActiveRigIndex + 1) % DiscoveredRigs.Num();
	UE_LOG(LogDisplayXRPreviewSession, Log, TEXT("DisplayXR Preview: Switched to rig: %s"),
		*GetActiveRigName());
}

FString FDisplayXRPreviewSession::GetActiveRigName() const
{
	if (ActiveRigIndex < 0 || ActiveRigIndex >= DiscoveredRigs.Num())
	{
		return TEXT("Editor Viewport");
	}

	const FRigEntry& Rig = DiscoveredRigs[ActiveRigIndex];
	UCameraComponent* Camera = Rig.Camera.Get();
	if (!Camera || !Camera->GetOwner()) return TEXT("(invalid)");

	FString TypeStr = Rig.CameraRig.IsValid() ? TEXT("Camera") : TEXT("Display");
	return FString::Printf(TEXT("%s [%s]"), *Camera->GetOwner()->GetActorLabel(), *TypeStr);
}

// =============================================================================
// Per-tick: locate views, compute stereo, capture scene, blit to swapchain
// =============================================================================

void FDisplayXRPreviewSession::RenderAndBlit(uint32_t ImageIndex)
{
	// -----------------------------------------------------------------------
	// This matches DisplayXRDevice::ComputeViews (display-centric path) exactly.
	// -----------------------------------------------------------------------

	// 1. Locate views to get raw eye positions (OpenXR display-local, meters)
	XrView Views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t ViewCount = 0;
	{
		XrViewLocateInfo LocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
		LocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		LocateInfo.displayTime = PredictedDisplayTime;
		LocateInfo.space = ViewSpace;
		XrViewState ViewState = {XR_TYPE_VIEW_STATE};
		xrLocateViewsFunc(Session, &LocateInfo, &ViewState, 2, &ViewCount, Views);
	}

	// -----------------------------------------------------------------------
	// Eye positions + Kooima: exact copy of DisplayXRDevice::ComputeViews
	// (display-centric path, lines 486-665 of DisplayXRDevice.cpp)
	// -----------------------------------------------------------------------

	// Read raw eye positions from xrLocateViews
	FVector LeftEyeRaw = FVector::ZeroVector, RightEyeRaw = FVector::ZeroVector;
	bool bTracked = false;
	if (ViewCount >= 2)
	{
		LeftEyeRaw = FVector(Views[0].pose.position.x, Views[0].pose.position.y, Views[0].pose.position.z);
		RightEyeRaw = FVector(Views[1].pose.position.x, Views[1].pose.position.y, Views[1].pose.position.z);
		bTracked = true;
	}

	// If no eye data yet, use nominal viewer + default IPD (exact same as game mode)
	bool bUsedFallback = false;
	if (LeftEyeRaw.IsNearlyZero() && RightEyeRaw.IsNearlyZero())
	{
		bUsedFallback = true;
		const float DefaultIPD = 0.063f;
		const float NomX = (float)DisplayInfo.NominalViewerPosition.X;
		const float NomY = (float)DisplayInfo.NominalViewerPosition.Y;
		const float NomZ = DisplayInfo.bIsValid ? (float)DisplayInfo.NominalViewerPosition.Z : 0.5f;
		LeftEyeRaw = FVector(NomX - DefaultIPD * 0.5f, NomY, NomZ);
		RightEyeRaw = FVector(NomX + DefaultIPD * 0.5f, NomY, NomZ);
	}

	XrVector3f RawEyes[2];
	RawEyes[0] = {(float)LeftEyeRaw.X, (float)LeftEyeRaw.Y, (float)LeftEyeRaw.Z};
	RawEyes[1] = {(float)RightEyeRaw.X, (float)RightEyeRaw.Y, (float)RightEyeRaw.Z};

	// -----------------------------------------------------------------------
	// Window-relative Kooima (matches openxr-3d-display test app pattern):
	// - Screen dimensions = physical window size in meters (not full display)
	// - Eye positions offset by distance from window center to display center
	// -----------------------------------------------------------------------
	float DispW_m = DisplayInfo.DisplayWidthMeters > 0.0f ? DisplayInfo.DisplayWidthMeters : 0.344f;
	float DispH_m = DisplayInfo.DisplayHeightMeters > 0.0f ? DisplayInfo.DisplayHeightMeters : 0.194f;
	float DispPxW = DisplayInfo.DisplayPixelWidth > 0 ? (float)DisplayInfo.DisplayPixelWidth : 3840.0f;
	float DispPxH = DisplayInfo.DisplayPixelHeight > 0 ? (float)DisplayInfo.DisplayPixelHeight : 2160.0f;
	float PxSizeX = DispW_m / DispPxW;
	float PxSizeY = DispH_m / DispPxH;

	// Default to full display
	Display3DScreen Screen;
	Screen.width_m = DispW_m;
	Screen.height_m = DispH_m;
	float EyeOffsetX_m = 0.0f;
	float EyeOffsetY_m = 0.0f;

#if PLATFORM_WINDOWS
	if (PreviewHWND)
	{
		// Window client area size in pixels
		RECT rc;
		GetClientRect((HWND)PreviewHWND, &rc);
		float WinPxW = (float)(rc.right - rc.left);
		float WinPxH = (float)(rc.bottom - rc.top);

		// Window physical size in meters
		if (WinPxW > 0 && WinPxH > 0)
		{
			Screen.width_m = WinPxW * PxSizeX;
			Screen.height_m = WinPxH * PxSizeY;
		}

		// Window center offset from display center (meters)
		POINT ClientOrigin = {0, 0};
		ClientToScreen((HWND)PreviewHWND, &ClientOrigin);
		HMONITOR hMon = MonitorFromWindow((HWND)PreviewHWND, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = {sizeof(mi)};
		if (GetMonitorInfo(hMon, &mi))
		{
			float WinCenterX = (float)(ClientOrigin.x - mi.rcMonitor.left) + WinPxW / 2.0f;
			float WinCenterY = (float)(ClientOrigin.y - mi.rcMonitor.top) + WinPxH / 2.0f;
			float MonW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
			float MonH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);

			EyeOffsetX_m = (WinCenterX - MonW / 2.0f) * PxSizeX;
			EyeOffsetY_m = -((WinCenterY - MonH / 2.0f) * PxSizeY);
		}
	}
#endif

	// Offset raw eyes by window center position (display-relative → window-relative)
	RawEyes[0].x -= EyeOffsetX_m;
	RawEyes[0].y -= EyeOffsetY_m;
	RawEyes[1].x -= EyeOffsetX_m;
	RawEyes[1].y -= EyeOffsetY_m;

	// Nominal viewer
	XrVector3f NominalViewer = {
		(float)DisplayInfo.NominalViewerPosition.X,
		(float)DisplayInfo.NominalViewerPosition.Y,
		(float)DisplayInfo.NominalViewerPosition.Z
	};
	if (!DisplayInfo.bIsValid)
	{
		NominalViewer = {0.0f, 0.0f, 0.5f};
	}
	// Also offset nominal viewer to window-relative
	NominalViewer.x -= EyeOffsetX_m;
	NominalViewer.y -= EyeOffsetY_m;

	// -----------------------------------------------------------------------
	// Read tunables + camera transform from active rig (or defaults if no rig)
	// Exact same as UDisplayXRCamera::PushTunables / UDisplayXRDisplay::PushTunables
	// -----------------------------------------------------------------------

	FVector CameraPos = FVector::ZeroVector;
	FRotator CameraRot = FRotator::ZeroRotator;
	bool bCameraCentric = false;
	float RigIpdFactor = 1.0f;
	float RigParallaxFactor = 1.0f;
	float RigPerspectiveFactor = 1.0f;
	float RigVirtualDisplayHeight = 0.0f;
	float RigInvConvergenceDistance = 0.0f;
	float RigFovOverride = 0.0f;

	bool bHasRig = false;
	if (ActiveRigIndex >= 0 && ActiveRigIndex < DiscoveredRigs.Num())
	{
		const FRigEntry& Rig = DiscoveredRigs[ActiveRigIndex];
		UCameraComponent* Camera = Rig.Camera.Get();
		if (Camera)
		{
			// If the rig is on a possessed pawn, use the player controller's
			// actual render POV (matches what UE's main viewport renders from).
			// This handles SpringArms, multiple cameras, camera-manager modifiers,
			// etc. — any case where the rig's UCameraComponent transform differs
			// from the active game-view POV. Fall back to the component transform
			// for unpossessed rigs.
			bool bUsedPOV = false;
			if (APawn* OwnerPawn = Cast<APawn>(Camera->GetOwner()))
			{
				if (AController* Ctrl = OwnerPawn->GetController())
				{
					FVector POVLoc; FRotator POVRot;
					Ctrl->GetPlayerViewPoint(POVLoc, POVRot);
					CameraPos = POVLoc;
					CameraRot = POVRot;
					bUsedPOV = true;
				}
			}
			if (!bUsedPOV)
			{
				CameraPos = Camera->GetComponentLocation();
				CameraRot = Camera->GetComponentRotation();
			}
			bHasRig = true;

			if (UDisplayXRCamera* CR = Rig.CameraRig.Get())
			{
				bCameraCentric = true;
				RigIpdFactor = CR->IpdFactor;
				RigParallaxFactor = CR->ParallaxFactor;
				RigInvConvergenceDistance = CR->InvConvergenceDistance;
				RigFovOverride = FMath::DegreesToRadians(Camera->FieldOfView);
			}
			else if (UDisplayXRDisplay* DR = Rig.DisplayRig.Get())
			{
				bCameraCentric = false;
				RigIpdFactor = DR->IpdFactor;
				RigParallaxFactor = DR->ParallaxFactor;
				RigPerspectiveFactor = DR->PerspectiveFactor;
				RigVirtualDisplayHeight = DR->VirtualDisplayHeight;
			}
		}
	}

	// Fallback: use editor viewport camera if no rig found
	if (!bHasRig)
	{
		FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
		if (LevelEditor)
		{
			TSharedPtr<IAssetViewport> ActiveViewport = LevelEditor->GetFirstActiveViewport();
			if (ActiveViewport.IsValid())
			{
				FEditorViewportClient& VC = ActiveViewport->GetAssetViewportClient();
				CameraPos = VC.GetViewLocation();
				CameraRot = VC.GetViewRotation();
			}
		}
	}

	// Diagnostic: log the active rig transform once per ~60 frames so we can
	// compare PIE vs game-mode values.
	if (FrameCount % 60 == 0)
	{
		UE_LOG(LogDisplayXRPreviewSession, Log,
			TEXT("DisplayXR Preview: rig='%s' cameraCentric=%d pos=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f)"),
			*GetActiveRigName(), bCameraCentric ? 1 : 0,
			CameraPos.X, CameraPos.Y, CameraPos.Z,
			CameraRot.Pitch, CameraRot.Yaw, CameraRot.Roll);
	}

	// Build scene transform from camera. Match game-mode DisplayXRDevice.cpp:552-560
	// exactly — UE coordinates are passed through to Kooima as-is (XrPosef fields
	// used as an opaque type, not a true OpenXR conversion). Converting via
	// UEOrientationToOpenXR here produces a different frame than game mode uses.
	const FQuat CamQuat = CameraRot.Quaternion();
	XrPosef DisplayPose;
	DisplayPose.position = {(float)CameraPos.X, (float)CameraPos.Y, (float)CameraPos.Z};
	DisplayPose.orientation = {(float)CamQuat.X, (float)CamQuat.Y, (float)CamQuat.Z, (float)CamQuat.W};

	// -----------------------------------------------------------------------
	// Compute stereo views — matching DisplayXRDevice::ComputeViews exactly
	// -----------------------------------------------------------------------

	FVector EyeOffsets[2];
	FMatrix ProjMatrices[2];

	if (bCameraCentric)
	{
		// Camera-centric path (same as DisplayXRDevice.cpp lines 567-608)
		Camera3DTunables CT;
		CT.ipd_factor = RigIpdFactor;
		CT.parallax_factor = RigParallaxFactor;
		CT.inv_convergence_distance = RigInvConvergenceDistance;
		CT.half_tan_vfov = RigFovOverride > 0.0f ? FMath::Tan(RigFovOverride * 0.5f) : 0.32491969623f;

		Camera3DView OutViews[2];
		camera3d_compute_views(RawEyes, 2, &NominalViewer, &Screen,
			&CT, &DisplayPose, 0.1f, 10000.0f, OutViews);

		// Get factored eyes for eye_local computation
		XrVector3f FactoredEyes[2];
		display3d_apply_eye_factors_n(RawEyes, 2, &NominalViewer,
			CT.ipd_factor, CT.parallax_factor, FactoredEyes);

		const float NomZ = (NominalViewer.z > 0.0f) ? NominalViewer.z : 0.5f;
		const float AspectRatio = (Screen.height_m > 0.0f) ? Screen.width_m / Screen.height_m : 1.78f;

		for (int32 i = 0; i < 2; i++)
		{
			XrVector3f EyeLocal = {
				FactoredEyes[i].x,
				FactoredEyes[i].y,
				FactoredEyes[i].z - NomZ
			};
			EyeOffsets[i] = OpenXRPositionToUE(EyeLocal);

			const float ConvergenceDist = RigInvConvergenceDistance > 0.0f
				? (1.0f / RigInvConvergenceDistance) * 100.0f : 100.0f;
			FVector2D HalfSize(ConvergenceDist * CT.half_tan_vfov * AspectRatio,
			                   ConvergenceDist * CT.half_tan_vfov);
			FVector EyeForProj(-ConvergenceDist, EyeOffsets[i].Y, EyeOffsets[i].Z);
			ProjMatrices[i] = CalculateOffAxisProjectionMatrix(HalfSize, EyeForProj);
		}
	}
	else
	{
		// Display-centric path (same as DisplayXRDevice.cpp lines 610-642)
		Display3DTunables DT;
		DT.ipd_factor = RigIpdFactor;
		DT.parallax_factor = RigParallaxFactor;
		DT.perspective_factor = RigPerspectiveFactor;
		// vdh is FIXED (does not scale with window). Screen.height_m is the window
		// physical height, so m2v = vdh / window_h changes with resize — making
		// objects smaller in a smaller window (same as test app / Unity).
		DT.virtual_display_height = RigVirtualDisplayHeight > 0.0f
			? RigVirtualDisplayHeight : DispH_m;

		Display3DView KooimaViews[2];
		display3d_compute_views(RawEyes, 2, &NominalViewer, &Screen,
			&DT, &DisplayPose, 0.1f, 10000.0f, KooimaViews);

		for (int32 i = 0; i < 2; i++)
		{
			EyeOffsets[i] = OpenXRPositionToUE(KooimaViews[i].eye_display);

			const float m2v = DT.virtual_display_height / Screen.height_m;
			const float ScreenW_UE = Screen.width_m * m2v * 100.0f;
			const float ScreenH_UE = DT.virtual_display_height * 100.0f;
			FVector2D HalfSize(ScreenW_UE * 0.5f, ScreenH_UE * 0.5f);

			ProjMatrices[i] = CalculateOffAxisProjectionMatrix(HalfSize, EyeOffsets[i]);
		}
	}

	// Diagnostic logging
	static int32 RenderCount = 0;
	RenderCount++;
	if (RenderCount <= 3 || RenderCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRPreviewSession, Log,
			TEXT("Preview #%d: winScreen=%.3fx%.3f m eyeOff=(%.4f,%.4f) m"),
			RenderCount, Screen.width_m, Screen.height_m, EyeOffsetX_m, EyeOffsetY_m);
		UE_LOG(LogDisplayXRPreviewSession, Log,
			TEXT("Preview #%d: rig=%s cameraCentric=%d pos=(%f,%f,%f) rot=(%f,%f,%f)"),
			RenderCount, *GetActiveRigName(), bCameraCentric,
			CameraPos.X, CameraPos.Y, CameraPos.Z,
			CameraRot.Pitch, CameraRot.Yaw, CameraRot.Roll);
		UE_LOG(LogDisplayXRPreviewSession, Log,
			TEXT("Preview #%d: offset[0]=(%f,%f,%f) offset[1]=(%f,%f,%f)"),
			RenderCount,
			EyeOffsets[0].X, EyeOffsets[0].Y, EyeOffsets[0].Z,
			EyeOffsets[1].X, EyeOffsets[1].Y, EyeOffsets[1].Z);
	}

	// Set capture transforms: rig camera + stereo eye offset
	// Same as CalculateStereoViewOffset: ViewLocation += ViewRotation.Quat.RotateVector(Offset)
	const FQuat CameraQuat = CameraRot.Quaternion();

	for (int32 i = 0; i < 2; i++)
	{
		FVector WorldPos = CameraPos + CameraQuat.RotateVector(EyeOffsets[i]);

		USceneCaptureComponent2D* Capture = (i == 0) ? LeftCapture : RightCapture;
		Capture->SetWorldLocationAndRotation(WorldPos, CameraRot);
		Capture->CustomProjectionMatrix = ProjMatrices[i];
	}

	// 6. Trigger captures
	LeftCapture->CaptureScene();
	RightCapture->CaptureScene();

	// 7. Blit render targets to swapchain image tiles (raw D3D12 copy, same format)
	if (ImageIndex < (uint32_t)SwapchainImages.Num())
	{
		const int32 TileW = ViewConfig.GetTileW();
		const int32 TileH = ViewConfig.GetTileH();

		FTextureResource* LeftRes = LeftRenderTarget->GetResource();
		FTextureResource* RightRes = RightRenderTarget->GetResource();
		void* DstD3DResource = SwapchainImages[ImageIndex].D3D12Resource;

		if (LeftRes && RightRes && DstD3DResource)
		{
			ENQUEUE_RENDER_COMMAND(BlitDisplayXRPreview)(
				[LeftRes, RightRes, DstD3DResource, TileW, TileH](FRHICommandListImmediate& RHICmdList)
				{
					FRHITexture* LeftSrc = LeftRes->TextureRHI;
					FRHITexture* RightSrc = RightRes->TextureRHI;
					if (!LeftSrc || !RightSrc) return;

					ID3D12Resource* SrcLeft = (ID3D12Resource*)LeftSrc->GetNativeResource();
					ID3D12Resource* SrcRight = (ID3D12Resource*)RightSrc->GetNativeResource();
					ID3D12Resource* Dst = (ID3D12Resource*)DstD3DResource;
					if (!SrcLeft || !SrcRight || !Dst) return;

					RHICmdList.Transition(FRHITransitionInfo(LeftSrc, ERHIAccess::Unknown, ERHIAccess::CopySrc));
					RHICmdList.Transition(FRHITransitionInfo(RightSrc, ERHIAccess::Unknown, ERHIAccess::CopySrc));

					RHICmdList.EnqueueLambda([SrcLeft, SrcRight, Dst, TileW, TileH](FRHICommandListBase& InRHICmdList)
					{
						ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
						if (!D3D12RHI) return;
						ID3D12GraphicsCommandList* CmdList = D3D12RHI->RHIGetGraphicsCommandList(InRHICmdList, 0);
						if (!CmdList) return;

						D3D12_TEXTURE_COPY_LOCATION DstLoc = {};
						DstLoc.pResource = Dst;
						DstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						DstLoc.SubresourceIndex = 0;

						D3D12_BOX SrcBox = {0, 0, 0, (UINT)TileW, (UINT)TileH, 1};

						// Copy left tile to (0, 0)
						D3D12_TEXTURE_COPY_LOCATION SrcLocL = {};
						SrcLocL.pResource = SrcLeft;
						SrcLocL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						SrcLocL.SubresourceIndex = 0;
						CmdList->CopyTextureRegion(&DstLoc, 0, 0, 0, &SrcLocL, &SrcBox);

						// Copy right tile to (TileW, 0)
						D3D12_TEXTURE_COPY_LOCATION SrcLocR = {};
						SrcLocR.pResource = SrcRight;
						SrcLocR.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						SrcLocR.SubresourceIndex = 0;
						CmdList->CopyTextureRegion(&DstLoc, TileW, 0, 0, &SrcLocR, &SrcBox);
					});
				});
			FlushRenderingCommands();
		}
	}
}
