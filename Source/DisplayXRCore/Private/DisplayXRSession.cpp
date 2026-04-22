// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRSession.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC || PLATFORM_LINUX
#include <dlfcn.h>
#endif

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRSession, Log, All);

FDisplayXRSession::FDisplayXRSession()
{
	FMemory::Memzero(TunablesBuffer, sizeof(TunablesBuffer));
	TunablesBuffer[0].IpdFactor = 1.0f;
	TunablesBuffer[0].ParallaxFactor = 1.0f;
	TunablesBuffer[0].PerspectiveFactor = 1.0f;
	TunablesBuffer[0].NearZ = 0.1f;
	TunablesBuffer[0].FarZ = 10000.0f;
	TunablesBuffer[1] = TunablesBuffer[0];

	// Default view config: 2D mode (1x1 tiles, full scale)
	ViewConfigBuffer[0] = FDisplayXRViewConfig();
	ViewConfigBuffer[1] = FDisplayXRViewConfig();
}

FDisplayXRSession::~FDisplayXRSession()
{
	if (bActive)
	{
		Shutdown();
	}
}

// =============================================================================
// Initialization
// =============================================================================

bool FDisplayXRSession::Initialize()
{
	if (bActive)
	{
		return true;
	}

	if (!LoadOpenXRLoader())
	{
		return false;
	}

	if (!CreateInstance())
	{
		UnloadOpenXRLoader();
		return false;
	}

	QueryDisplayInfo();

	// Initialize view config.
	// displayPixelWidth/Height from XR_EXT_display_info SHOULD be the physical
	// resolution (3840x2160), but some runtimes report DPI-scaled logical pixels.
	// If session creates successfully, QueryRenderingModes will override with
	// accurate dimensions from viewWidthPixels/viewScaleX.
	// For now, start in 3D mode (2x1 tiles, scale 0.5x0.5) as the default
	// since that's the mode the display will be in.
	{
		const int32 WriteIdx = 1 - ViewConfigReadIndex.Load();
		FDisplayXRViewConfig& VC = ViewConfigBuffer[WriteIdx];
		VC.DisplayPixelW = DisplayInfo.DisplayPixelWidth > 0 ? DisplayInfo.DisplayPixelWidth : 3840;
		VC.DisplayPixelH = DisplayInfo.DisplayPixelHeight > 0 ? DisplayInfo.DisplayPixelHeight : 2160;
		// Default to 3D mode layout
		VC.TileColumns = 2;
		VC.TileRows = 1;
		VC.ScaleX = 0.5f;
		VC.ScaleY = 0.5f;
		ViewConfigReadIndex.Store(WriteIdx);

		UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: ViewConfig initialized — %dx%d tiles, scale %.2fx%.2f, display %dx%d px"),
			VC.TileColumns, VC.TileRows, VC.ScaleX, VC.ScaleY, VC.DisplayPixelW, VC.DisplayPixelH);
	}

	if (!CreateSession())
	{
		UE_LOG(LogDisplayXRSession, Warning, TEXT("DisplayXR Session: Session creation failed, will retry"));
	}

	bActive = true;
	UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Initialized"));
	return true;
}

void FDisplayXRSession::Shutdown()
{
	if (Session != XR_NULL_HANDLE)
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

	if (ViewSpace != XR_NULL_HANDLE)
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

	if (Instance != XR_NULL_HANDLE)
	{
		PFN_xrDestroyInstance xrDestroyInstanceFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrDestroyInstance",
			(PFN_xrVoidFunction*)&xrDestroyInstanceFunc);
		if (xrDestroyInstanceFunc)
		{
			xrDestroyInstanceFunc(Instance);
		}
		Instance = XR_NULL_HANDLE;
	}

	UnloadOpenXRLoader();
	bActive = false;
	bSessionRunning = false;
	UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Shut down"));
}

// =============================================================================
// OpenXR loader
// =============================================================================

bool FDisplayXRSession::LoadOpenXRLoader()
{
#if PLATFORM_WINDOWS
	// --- Strategy: load the DisplayXR runtime DLL directly via negotiate ---
	// This gives us the in-process compositor (weaving happens in our process)
	// rather than going through openxr_loader.dll → DisplayXRClient.dll (IPC).

	// Negotiate function types
	struct XrNegotiateLoaderInfo
	{
		uint32_t structType;       // XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1
		uint32_t structVersion;
		size_t structSize;
		uint32_t minInterfaceVersion;
		uint32_t maxInterfaceVersion;
		uint64_t minApiVersion;    // XrVersion = uint64_t
		uint64_t maxApiVersion;
	};
	struct XrNegotiateRuntimeRequest
	{
		uint32_t structType;       // XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3
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
		// Try XR_RUNTIME_JSON env var first
		FString RuntimeJsonPath = FPlatformMisc::GetEnvironmentVariable(TEXT("XR_RUNTIME_JSON"));

		// Fallback: registry ActiveRuntime (direct Win32 API for reliability)
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
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Runtime JSON: %s"), *RuntimeJsonPath);

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
							// library_path may be relative to the JSON file's directory
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

	// Step 2: Try loading runtime DLL directly and negotiating
	if (!RuntimeDllPath.IsEmpty())
	{
		LoaderHandle = (void*)LoadLibraryW(*RuntimeDllPath);
		if (LoaderHandle)
		{
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Loaded runtime DLL: %s"), *RuntimeDllPath);

			auto NegotiateFunc = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
				reinterpret_cast<void*>(GetProcAddress((HMODULE)LoaderHandle, "xrNegotiateLoaderRuntimeInterface")));

			if (NegotiateFunc)
			{
				XrNegotiateLoaderInfo LoaderInfo = {};
				LoaderInfo.structType = 1; // XR_LOADER_INTERFACE_STRUCT_LOADER_INFO
				LoaderInfo.structVersion = 1;
				LoaderInfo.structSize = sizeof(LoaderInfo);
				LoaderInfo.minInterfaceVersion = 1;
				LoaderInfo.maxInterfaceVersion = 1;
				LoaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
				LoaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

				XrNegotiateRuntimeRequest RuntimeRequest = {};
				RuntimeRequest.structType = 3; // XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST
				RuntimeRequest.structVersion = 1;
				RuntimeRequest.structSize = sizeof(RuntimeRequest);

				XrResult Result = NegotiateFunc(&LoaderInfo, &RuntimeRequest);
				if (XR_SUCCEEDED(Result) && RuntimeRequest.getInstanceProcAddr)
				{
					xrGetInstanceProcAddrFunc = RuntimeRequest.getInstanceProcAddr;
					UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Got xrGetInstanceProcAddr via negotiate (in-process compositor)"));
					return true;
				}
				else
				{
					UE_LOG(LogDisplayXRSession, Warning, TEXT("DisplayXR Session: Negotiate failed (%d), trying xrGetInstanceProcAddr directly"), (int)Result);
				}
			}

			// Fallback: try direct xrGetInstanceProcAddr export
			xrGetInstanceProcAddrFunc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
				reinterpret_cast<void*>(GetProcAddress((HMODULE)LoaderHandle, "xrGetInstanceProcAddr")));
			if (xrGetInstanceProcAddrFunc)
			{
				UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Got xrGetInstanceProcAddr directly from runtime DLL"));
				return true;
			}

			// Runtime DLL loaded but no usable entry point — unload and try fallback
			FreeLibrary((HMODULE)LoaderHandle);
			LoaderHandle = nullptr;
		}
		else
		{
			UE_LOG(LogDisplayXRSession, Warning, TEXT("DisplayXR Session: Failed to load runtime DLL: %s"), *RuntimeDllPath);
		}
	}

	// Step 3: Fallback — try loader DLLs (IPC path, less preferred)
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
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Loaded OpenXR fallback from %s"), FallbackPaths[i]);
			break;
		}
	}

	if (!LoaderHandle)
	{
		UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: Failed to load any OpenXR runtime or loader"));
		return false;
	}

	xrGetInstanceProcAddrFunc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
		reinterpret_cast<void*>(GetProcAddress((HMODULE)LoaderHandle, "xrGetInstanceProcAddr")));

	if (!xrGetInstanceProcAddrFunc)
	{
		UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: No xrGetInstanceProcAddr in loader"));
		UnloadOpenXRLoader();
		return false;
	}

#elif PLATFORM_MAC || PLATFORM_LINUX
	const char* LoaderPaths[] = {
		"/usr/local/lib/libopenxr_displayxr.so",
		"libopenxr_loader.dylib",
		"libopenxr_loader.so",
		nullptr
	};

	for (int i = 0; LoaderPaths[i] != nullptr; i++)
	{
		LoaderHandle = dlopen(LoaderPaths[i], RTLD_NOW | RTLD_LOCAL);
		if (LoaderHandle)
		{
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Loaded OpenXR from %s"),
				ANSI_TO_TCHAR(LoaderPaths[i]));
			break;
		}
	}

	if (!LoaderHandle)
	{
		UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: Failed to load OpenXR loader"));
		return false;
	}

	xrGetInstanceProcAddrFunc = (PFN_xrGetInstanceProcAddr)dlsym(LoaderHandle, "xrGetInstanceProcAddr");
	if (!xrGetInstanceProcAddrFunc)
	{
		// Try the negotiate function for runtime-as-loader pattern
		typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(const void*, void*);
		auto negotiateFunc = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(
			LoaderHandle, "xrNegotiateLoaderRuntimeInterface");

		if (!negotiateFunc)
		{
			UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: No xrGetInstanceProcAddr in loader"));
			UnloadOpenXRLoader();
			return false;
		}

		struct XrNegotiateLoaderInfo
		{
			uint32_t structType;
			uint32_t structVersion;
			size_t structSize;
			uint32_t minInterfaceVersion;
			uint32_t maxInterfaceVersion;
			uint32_t minApiVersion;
			uint32_t maxApiVersion;
		};
		struct XrNegotiateRuntimeRequest
		{
			uint32_t structType;
			uint32_t structVersion;
			size_t structSize;
			uint32_t runtimeInterfaceVersion;
			uint32_t runtimeApiVersion;
			PFN_xrGetInstanceProcAddr getInstanceProcAddr;
		};

		XrNegotiateLoaderInfo loaderInfo = {};
		loaderInfo.structType = 1;
		loaderInfo.structVersion = 1;
		loaderInfo.structSize = sizeof(loaderInfo);
		loaderInfo.minInterfaceVersion = 1;
		loaderInfo.maxInterfaceVersion = 1;
		loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
		loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 1, 0);

		XrNegotiateRuntimeRequest runtimeRequest = {};
		runtimeRequest.structType = 3;
		runtimeRequest.structVersion = 1;
		runtimeRequest.structSize = sizeof(runtimeRequest);

		XrResult result = negotiateFunc(&loaderInfo, &runtimeRequest);
		if (XR_SUCCEEDED(result) && runtimeRequest.getInstanceProcAddr)
		{
			xrGetInstanceProcAddrFunc = runtimeRequest.getInstanceProcAddr;
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Got xrGetInstanceProcAddr via negotiate"));
		}
		else
		{
			UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: Negotiate failed"));
			UnloadOpenXRLoader();
			return false;
		}
	}
#else
	UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: Platform not supported"));
	return false;
#endif

	return true;
}

void FDisplayXRSession::UnloadOpenXRLoader()
{
	if (LoaderHandle)
	{
#if PLATFORM_WINDOWS
		FreeLibrary((HMODULE)LoaderHandle);
#elif PLATFORM_MAC || PLATFORM_LINUX
		dlclose(LoaderHandle);
#endif
		LoaderHandle = nullptr;
	}
	xrGetInstanceProcAddrFunc = nullptr;
}

// =============================================================================
// Instance + Session
// =============================================================================

bool FDisplayXRSession::CreateInstance()
{
	PFN_xrCreateInstance xrCreateInstanceFunc = nullptr;
	xrGetInstanceProcAddrFunc(XR_NULL_HANDLE, "xrCreateInstance",
		(PFN_xrVoidFunction*)&xrCreateInstanceFunc);
	if (!xrCreateInstanceFunc)
	{
		UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: No xrCreateInstance"));
		return false;
	}

	const char* Extensions[] = {
		XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
#if PLATFORM_WINDOWS
		"XR_KHR_D3D12_enable",
		XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME,
#elif PLATFORM_MAC
		XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME,
#endif
	};

	XrInstanceCreateInfo CreateInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	FCStringAnsi::Strncpy(CreateInfo.applicationInfo.applicationName, "DisplayXR Unreal Plugin", XR_MAX_APPLICATION_NAME_SIZE);
	CreateInfo.applicationInfo.applicationVersion = 1;
	FCStringAnsi::Strncpy(CreateInfo.applicationInfo.engineName, "Unreal Engine", XR_MAX_ENGINE_NAME_SIZE);
	CreateInfo.applicationInfo.engineVersion = 5;
	CreateInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	CreateInfo.enabledExtensionCount = UE_ARRAY_COUNT(Extensions);
	CreateInfo.enabledExtensionNames = Extensions;

	XrResult Result = xrCreateInstanceFunc(&CreateInfo, &Instance);
	if (!XR_SUCCEEDED(Result))
	{
		UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: xrCreateInstance failed (%d)"), (int)Result);
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
		UE_LOG(LogDisplayXRSession, Error, TEXT("DisplayXR Session: xrGetSystem failed (%d)"), (int)Result);
		return false;
	}

	UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Instance created, SystemId=%llu"), (unsigned long long)SystemId);
	return true;
}

void FDisplayXRSession::QueryDisplayInfo()
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

		UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Display %.3f x %.3f m, %d x %d px"),
			DisplayInfo.DisplayWidthMeters, DisplayInfo.DisplayHeightMeters,
			DisplayInfo.DisplayPixelWidth, DisplayInfo.DisplayPixelHeight);
	}
}

void FDisplayXRSession::QueryRenderingModes()
{
	if (!xrEnumerateDisplayRenderingModesFunc || Session == XR_NULL_HANDLE)
	{
		UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: No rendering mode enumeration available"));
		return;
	}

	uint32_t ModeCount = 0;
	XrResult Result = xrEnumerateDisplayRenderingModesFunc(Session, 0, &ModeCount, nullptr);
	if (!XR_SUCCEEDED(Result) || ModeCount == 0)
	{
		UE_LOG(LogDisplayXRSession, Warning, TEXT("DisplayXR Session: No rendering modes found (%d)"), (int)Result);
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

	// Log all available modes and find the 3D mode
	for (uint32_t i = 0; i < ModeCount; i++)
	{
		const auto& M = Modes[i];
		UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Mode[%d] '%s' views=%d scale=%.2fx%.2f tiles=%dx%d viewPx=%dx%d hw3D=%d"),
			M.modeIndex, ANSI_TO_TCHAR(M.modeName), M.viewCount,
			M.viewScaleX, M.viewScaleY, M.tileColumns, M.tileRows,
			M.viewWidthPixels, M.viewHeightPixels, (int)M.hardwareDisplay3D);

		// Use the 3D mode (hardwareDisplay3D=true) to populate ViewConfig
		if (M.hardwareDisplay3D && M.viewCount >= 2)
		{
			const int32 WriteIdx = 1 - ViewConfigReadIndex.Load();
			FDisplayXRViewConfig& VC = ViewConfigBuffer[WriteIdx];
			VC.TileColumns = (int32)M.tileColumns;
			VC.TileRows = (int32)M.tileRows;
			VC.ScaleX = M.viewScaleX;
			VC.ScaleY = M.viewScaleY;
			// Use viewWidthPixels/viewHeightPixels to derive physical display dimensions
			if (M.viewWidthPixels > 0 && M.viewHeightPixels > 0 && M.viewScaleX > 0.0f && M.viewScaleY > 0.0f)
			{
				VC.DisplayPixelW = (int32)(M.viewWidthPixels / M.viewScaleX);
				VC.DisplayPixelH = (int32)(M.viewHeightPixels / M.viewScaleY);
			}
			ViewConfigReadIndex.Store(WriteIdx);

			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Using 3D mode — %dx%d tiles, scale %.2fx%.2f, display %dx%d px"),
				VC.TileColumns, VC.TileRows, VC.ScaleX, VC.ScaleY, VC.DisplayPixelW, VC.DisplayPixelH);
		}
	}
}

bool FDisplayXRSession::CreateSession()
{
	PFN_xrCreateSession xrCreateSessionFunc = nullptr;
	xrGetInstanceProcAddrFunc(Instance, "xrCreateSession",
		(PFN_xrVoidFunction*)&xrCreateSessionFunc);
	if (!xrCreateSessionFunc)
	{
		return false;
	}

	XrSessionCreateInfo SessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
	SessionCreateInfo.systemId = SystemId;
	// No graphics binding chain — the runtime will use its own compositor.
	// HWND can be provided later via xrSetSharedTextureOutputRectEXT or
	// by re-creating the session once the game window is available.

	XrResult Result = xrCreateSessionFunc(Instance, &SessionCreateInfo, &Session);
	if (!XR_SUCCEEDED(Result))
	{
		UE_LOG(LogDisplayXRSession, Warning, TEXT("DisplayXR Session: xrCreateSession failed (%d)"), (int)Result);
		return false;
	}

	// Create LOCAL reference space for xrLocateViews
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

	// Query rendering modes to get tile layout and view dimensions
	QueryRenderingModes();

	UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Session created"));
	return true;
}

// =============================================================================
// Per-frame tick
// =============================================================================

void FDisplayXRSession::Tick()
{
	if (!bActive || Instance == XR_NULL_HANDLE || Session == XR_NULL_HANDLE)
	{
		return;
	}

	// Skip xrPollEvent entirely once the session is running and the compositor
	// thread is active. The runtime's xrPollEvent blocks on the game thread
	// because the in-process compositor expects all interaction via xrWaitFrame.
	static int32 SkipPollCount = 0;
	if (SkipPollCount > 0)
	{
		SkipPollCount--;
	}
	else if (!bSessionRunning)
	{
		// Only poll events during session setup (before RUNNING state).
		// Once running, the compositor thread handles the runtime.
		PFN_xrPollEvent xrPollEventFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrPollEvent", (PFN_xrVoidFunction*)&xrPollEventFunc);
		if (xrPollEventFunc)
		{
			XrEventDataBuffer EventData = {XR_TYPE_EVENT_DATA_BUFFER};
			while (XR_SUCCEEDED(xrPollEventFunc(Instance, &EventData)))
			{
				if (EventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					auto* StateChanged = (XrEventDataSessionStateChanged*)&EventData;
					SessionState = StateChanged->state;

					if (SessionState == XR_SESSION_STATE_READY)
					{
						PFN_xrBeginSession xrBeginSessionFunc = nullptr;
						xrGetInstanceProcAddrFunc(Instance, "xrBeginSession",
							(PFN_xrVoidFunction*)&xrBeginSessionFunc);
						if (xrBeginSessionFunc)
						{
							XrSessionBeginInfo BeginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
							BeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
							UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Calling xrBeginSession..."));
							GLog->Flush();
							XrResult BeginResult = xrBeginSessionFunc(Session, &BeginInfo);
							UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: xrBeginSession returned %d"), (int)BeginResult);
							GLog->Flush();
							bSessionRunning = true;
							UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Session running"));
							GLog->Flush();
							// Skip xrPollEvent for several ticks to let the compositor
							// thread start its frame loop before we poll again.
							SkipPollCount = 60;
							break; // Exit the while loop
						}
					}
					else if (SessionState == XR_SESSION_STATE_STOPPING)
					{
						PFN_xrEndSession xrEndSessionFunc = nullptr;
						xrGetInstanceProcAddrFunc(Instance, "xrEndSession",
							(PFN_xrVoidFunction*)&xrEndSessionFunc);
						if (xrEndSessionFunc)
						{
							xrEndSessionFunc(Session);
						}
						bSessionRunning = false;
					}
				}
				EventData = {XR_TYPE_EVENT_DATA_BUFFER};
			}
		}
	}

	// Locate views for eye tracking data
	if (bSessionRunning && Session != XR_NULL_HANDLE && ViewSpace != XR_NULL_HANDLE)
	{
		static bool bFirstLocate = true;
		if (bFirstLocate)
		{
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: First LocateViews call..."));
			GLog->Flush();
		}
		LocateViews();
		if (bFirstLocate)
		{
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: First LocateViews returned"));
			GLog->Flush();
			bFirstLocate = false;
		}
	}
}

void FDisplayXRSession::LocateViews()
{
	PFN_xrLocateViews xrLocateViewsFunc = nullptr;
	xrGetInstanceProcAddrFunc(Instance, "xrLocateViews",
		(PFN_xrVoidFunction*)&xrLocateViewsFunc);
	if (!xrLocateViewsFunc)
	{
		return;
	}

	XrViewLocateInfo LocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
	LocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	LocateInfo.displayTime = (XrTime)PredictedDisplayTime.Load();
	LocateInfo.space = ViewSpace;

	XrViewState ViewState = {XR_TYPE_VIEW_STATE};
	XrView Views[8];
	for (int i = 0; i < 8; i++) Views[i] = {XR_TYPE_VIEW};
	uint32_t ViewCount = 0;

	XrResult Result = xrLocateViewsFunc(Session, &LocateInfo, &ViewState, 8, &ViewCount, Views);

	// Diagnostic: log even on failure
	static int32 LocateFailCount = 0;
	if (!XR_SUCCEEDED(Result) || ViewCount == 0)
	{
		LocateFailCount++;
		if (LocateFailCount <= 3 || LocateFailCount % 300 == 0)
		{
			UE_LOG(LogDisplayXRSession, Warning,
				TEXT("LocateViews FAILED #%d: result=%d viewCount=%u session=%llu space=%llu displayTime=%lld"),
				LocateFailCount, (int)Result, ViewCount,
				(unsigned long long)Session, (unsigned long long)LocateInfo.space,
				(long long)LocateInfo.displayTime);
			GLog->Flush();
		}
		return;
	}

	// Diagnostic: log raw xrLocateViews output periodically
	static int32 LocateCount = 0;
	LocateCount++;
	if (LocateCount <= 3 || LocateCount % 300 == 0)
	{
		uint64 Flags = ViewState.viewStateFlags;
		bool bPosTrk = (Flags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
		bool bPosVal = (Flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
		UE_LOG(LogDisplayXRSession, Log,
			TEXT("LocateViews #%d: result=%d views=%u flags=0x%llx posValid=%d posTracked=%d displayTime=%lld"),
			LocateCount, (int)Result, ViewCount, (unsigned long long)Flags, bPosVal, bPosTrk, (long long)LocateInfo.displayTime);
		if (ViewCount >= 2)
		{
			UE_LOG(LogDisplayXRSession, Log,
				TEXT("  Eye[0]=(%f, %f, %f) Eye[1]=(%f, %f, %f)"),
				Views[0].pose.position.x, Views[0].pose.position.y, Views[0].pose.position.z,
				Views[1].pose.position.x, Views[1].pose.position.y, Views[1].pose.position.z);
		}
		GLog->Flush();
	}

	// Store eye positions (in OpenXR display-local space, meters)
	const bool bTracked = (ViewState.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
	{
		const int32 WriteIdx = 1 - EyeDataReadIndex.Load();
		FEyeData& ED = EyeDataBuffer[WriteIdx];
		if (ViewCount >= 2)
		{
			ED.LeftEye = FVector(Views[0].pose.position.x, Views[0].pose.position.y, Views[0].pose.position.z);
			ED.RightEye = FVector(Views[1].pose.position.x, Views[1].pose.position.y, Views[1].pose.position.z);
		}
		else if (ViewCount >= 1)
		{
			ED.LeftEye = FVector(Views[0].pose.position.x, Views[0].pose.position.y, Views[0].pose.position.z);
			ED.RightEye = ED.LeftEye;
		}
		ED.bTracked = bTracked;
		EyeDataReadIndex.Store(WriteIdx);
	}
}

void FDisplayXRSession::UpdateViewConfigFromDisplayMode(bool bMode3D)
{
	const int32 ReadIdx = ViewConfigReadIndex.Load();
	const FDisplayXRViewConfig& Current = ViewConfigBuffer[ReadIdx];

	const int32 WriteIdx = 1 - ReadIdx;
	FDisplayXRViewConfig& VC = ViewConfigBuffer[WriteIdx];

	// Preserve display pixel dimensions
	VC.DisplayPixelW = Current.DisplayPixelW;
	VC.DisplayPixelH = Current.DisplayPixelH;

	if (bMode3D)
	{
		// 3D mode: 2 columns, 1 row, each view at scale 0.5x0.5
		// Atlas covers the top half of the swapchain (two half-res views side by side)
		VC.TileColumns = 2;
		VC.TileRows = 1;
		VC.ScaleX = 0.5f;
		VC.ScaleY = 0.5f;
	}
	else
	{
		// 2D mode: 1x1 tiles @ scale 1x1
		VC.TileColumns = 1;
		VC.TileRows = 1;
		VC.ScaleX = 1.0f;
		VC.ScaleY = 1.0f;
	}

	ViewConfigReadIndex.Store(WriteIdx);
}

// =============================================================================
// Public API
// =============================================================================

FDisplayXRViewConfig FDisplayXRSession::GetViewConfig() const
{
	const int32 ReadIdx = ViewConfigReadIndex.Load();
	return ViewConfigBuffer[ReadIdx];
}

void FDisplayXRSession::GetEyePositions(FVector& OutLeft, FVector& OutRight, bool& bOutTracked) const
{
	const int32 ReadIdx = EyeDataReadIndex.Load();
	const FEyeData& ED = EyeDataBuffer[ReadIdx];
	OutLeft = ED.LeftEye;
	OutRight = ED.RightEye;
	bOutTracked = ED.bTracked;
}

bool FDisplayXRSession::RequestDisplayMode(bool bMode3D)
{
	if (xrRequestDisplayModeFunc && Session != XR_NULL_HANDLE)
	{
		XrDisplayModeEXT Mode = bMode3D ? XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
		XrResult Result = xrRequestDisplayModeFunc(Session, Mode);
		if (XR_SUCCEEDED(Result))
		{
			UpdateViewConfigFromDisplayMode(bMode3D);
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Display mode set to %s"),
				bMode3D ? TEXT("3D") : TEXT("2D"));
			return true;
		}
	}
	return false;
}

void FDisplayXRSession::SetTunables(const FDisplayXRTunables& InTunables)
{
	const int32 WriteIdx = 1 - TunablesReadIndex.Load();
	TunablesBuffer[WriteIdx] = InTunables;
	TunablesReadIndex.Store(WriteIdx);
}

FDisplayXRTunables FDisplayXRSession::GetTunables() const
{
	const int32 ReadIdx = TunablesReadIndex.Load();
	return TunablesBuffer[ReadIdx];
}

bool FDisplayXRSession::CreateSessionWithGraphics(void* D3DDevice, void* CommandQueue, void* WindowHandle)
{
	if (Session != XR_NULL_HANDLE)
	{
		// Destroy existing bare session to replace with graphics-bound session
		UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Destroying existing session to create one with graphics binding"));

		if (bSessionRunning)
		{
			PFN_xrEndSession xrEndSessionFunc = nullptr;
			xrGetInstanceProcAddrFunc(Instance, "xrEndSession",
				(PFN_xrVoidFunction*)&xrEndSessionFunc);
			if (xrEndSessionFunc)
			{
				xrEndSessionFunc(Session);
			}
			bSessionRunning = false;
		}

		if (ViewSpace != XR_NULL_HANDLE)
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

		PFN_xrDestroySession xrDestroySessionFunc = nullptr;
		xrGetInstanceProcAddrFunc(Instance, "xrDestroySession",
			(PFN_xrVoidFunction*)&xrDestroySessionFunc);
		if (xrDestroySessionFunc)
		{
			xrDestroySessionFunc(Session);
		}
		Session = XR_NULL_HANDLE;
		SessionState = XR_SESSION_STATE_UNKNOWN;
	}

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

	// D3D12 graphics binding (required by the runtime)
	struct XrGraphicsBindingD3D12KHR
	{
		XrStructureType type;
		const void* next;
		void* device;       // ID3D12Device*
		void* queue;        // ID3D12CommandQueue*
	};

	XrGraphicsBindingD3D12KHR D3D12Binding = {};
	D3D12Binding.type = (XrStructureType)XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
	D3D12Binding.device = D3DDevice;
	D3D12Binding.queue = CommandQueue;

	// Chain Win32 window binding if HWND is available
	XrWin32WindowBindingCreateInfoEXT Win32Binding = {};
	Win32Binding.type = (XrStructureType)XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT;
	Win32Binding.windowHandle = WindowHandle;

	if (WindowHandle)
	{
		D3D12Binding.next = &Win32Binding;
	}

	XrSessionCreateInfo SessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
	SessionCreateInfo.systemId = SystemId;
	SessionCreateInfo.next = &D3D12Binding;

	// Call xrGetD3D12GraphicsRequirementsKHR (required before xrCreateSession)
	{
		typedef struct XrGraphicsRequirementsD3D12KHR
		{
			XrStructureType type;
			void* next;
			int64_t adapterLuid;  // LUID
			int32_t minFeatureLevel; // D3D_FEATURE_LEVEL
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
			UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: D3D12 graphics requirements queried (minFeatureLevel=%d)"),
				Reqs.minFeatureLevel);
		}
	}

	UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Creating session with D3D12 device=%p queue=%p hwnd=%p"),
		D3DDevice, CommandQueue, WindowHandle);

	XrResult Result = xrCreateSessionFunc(Instance, &SessionCreateInfo, &Session);
	if (!XR_SUCCEEDED(Result))
	{
		UE_LOG(LogDisplayXRSession, Warning, TEXT("DisplayXR Session: xrCreateSession with graphics failed (%d)"), (int)Result);
		return false;
	}

	// Create reference space
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

	QueryRenderingModes();

	UE_LOG(LogDisplayXRSession, Log, TEXT("DisplayXR Session: Session created with graphics binding"));
	return true;
}

void FDisplayXRSession::SetSceneTransform(const FTransform& InTransform, bool bEnabled)
{
	const int32 WriteIdx = 1 - SceneTransformReadIndex.Load();
	FSceneTransformData& ST = SceneTransformBuffer[WriteIdx];
	ST.Position = InTransform.GetLocation();
	ST.Orientation = InTransform.GetRotation();
	ST.Scale = InTransform.GetScale3D();
	ST.bEnabled = bEnabled;
	SceneTransformReadIndex.Store(WriteIdx);
}

void FDisplayXRSession::GetSceneTransform(FVector& OutPosition, FQuat& OutOrientation, bool& bOutEnabled) const
{
	const int32 ReadIdx = SceneTransformReadIndex.Load();
	const FSceneTransformData& ST = SceneTransformBuffer[ReadIdx];
	OutPosition = ST.Position;
	OutOrientation = ST.Orientation;
	bOutEnabled = ST.bEnabled;
}
