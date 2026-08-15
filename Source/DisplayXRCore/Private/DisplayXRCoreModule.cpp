// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRCoreModule.h"
#include "DisplayXRAtlasCapture.h"
#include "DisplayXRSession.h"
#include "DisplayXRPlatform.h"
#include "Rendering/DisplayXRDevice.h"
#include "SceneViewExtension.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Engine.h"
#include "RenderingThread.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"
#include "ID3D12DynamicRHI.h"
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellscalingapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#pragma comment(lib, "Shcore.lib")
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRCore, Log, All);

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// Hide UE's own top-level desktop windows under the shell, from a thread that is
// INDEPENDENT of the game thread. During UE's BLOCKING startup load (shaders /
// level) the game thread is stalled, so the compositor loop — which normally hides
// the window — hasn't started yet, and UE's black window covers the shell + other
// apps for several seconds. This watcher clips every top-level visible window owned
// by our process to an empty region the moment it appears (and hands OS foreground
// back to the shell once), regardless of the game thread's state. Cross-thread
// SetWindowRgn is allowed and does not require the owning thread to pump messages.
// Clipping only hides the desktop composite — UE keeps rendering to the swapchain.
class FDisplayXRWindowHider : public FRunnable
{
public:
	explicit FDisplayXRWindowHider(void* InShellHWND) : ShellHWND(InShellHWND) {}
	virtual bool Init() override { return true; }
	virtual uint32 Run() override
	{
		bool bHandedBack = false;
		int Iter = 0;
		while (!bStop)
		{
			const int Hid = HideProcessTopLevelWindows();
			if (Hid > 0 && !bHandedBack && ShellHWND && ::IsWindow((HWND)ShellHWND))
			{
				::SetForegroundWindow((HWND)ShellHWND);
				bHandedBack = true;
			}
			// Poll fast during the startup-load window, then back off to keep
			// steady-state overhead negligible (still catches a later re-show).
			::Sleep(Iter < 1000 ? 30 : 500);
			++Iter;
		}
		return 0;
	}
	virtual void Stop() override { bStop = true; }

private:
	static int HideProcessTopLevelWindows()
	{
		struct FCtx { DWORD Pid; int Hid; } Ctx{ ::GetCurrentProcessId(), 0 };
		::EnumWindows([](HWND Hwnd, LPARAM Lp) -> BOOL
		{
			FCtx* C = reinterpret_cast<FCtx*>(Lp);
			DWORD WPid = 0; ::GetWindowThreadProcessId(Hwnd, &WPid);
			if (WPid != C->Pid) return 1;                      // other processes
			if (::GetWindow(Hwnd, GW_OWNER) != nullptr) return 1; // owned popups
			if (!::IsWindowVisible(Hwnd)) return 1;
			RECT Box; const int T = ::GetWindowRgnBox(Hwnd, &Box);
			if (T == NULLREGION) return 1;                     // already clipped empty
			::SetWindowRgn(Hwnd, ::CreateRectRgn(0, 0, 0, 0), true);
			++C->Hid;
			return 1;
		}, reinterpret_cast<LPARAM>(&Ctx));
		return Ctx.Hid;
	}
	FThreadSafeBool bStop{ false };
	void* ShellHWND = nullptr;
};

static FDisplayXRWindowHider* GWindowHider = nullptr;
static FRunnableThread* GWindowHiderThread = nullptr;
#endif

// =============================================================================
// Dev-mode input preprocessor: SHIFT+F1 toggles mouse cursor / input mode on
// the local PlayerController. Matches the editor's SHIFT+F1 affordance so
// developers can release the game's mouse-look without having to Alt+F4 out.
// =============================================================================
class FDisplayXRDevInputProcessor : public IInputProcessor
{
public:
	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& KeyEvent) override
	{
		// 'I' (no modifiers) — capture atlas to PNG, parity with runtime-pvt
		// reference test apps. Skip when typing into a text widget.
		if (KeyEvent.GetKey() == EKeys::I
			&& !KeyEvent.IsShiftDown() && !KeyEvent.IsControlDown() && !KeyEvent.IsAltDown())
		{
			const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(0);
			const bool bTextFocus = Focused.IsValid() && Focused->GetType().ToString().Contains(TEXT("EditableText"));
			if (!bTextFocus)
			{
				FDisplayXRAtlasCapture::RequestCapture();
				return true;
			}
		}

		if (KeyEvent.GetKey() != EKeys::F1 || !KeyEvent.IsShiftDown())
		{
			return false;
		}
		APlayerController* PC = FindLocalPC();
		if (!PC)
		{
			return false;
		}
		bCaptured = !bCaptured;
		if (bCaptured)
		{
			// Return to game: hide cursor, lock, game-only input.
			FInputModeGameOnly InputMode;
			PC->SetInputMode(InputMode);
			PC->SetShowMouseCursor(false);
			UE_LOG(LogTemp, Log, TEXT("DisplayXR: SHIFT+F1 → mouse captured (game)"));
		}
		else
		{
			// Release: show cursor, unlock, game+UI input.
			FInputModeGameAndUI InputMode;
			InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			InputMode.SetHideCursorDuringCapture(false);
			PC->SetInputMode(InputMode);
			PC->SetShowMouseCursor(true);
			UE_LOG(LogTemp, Log, TEXT("DisplayXR: SHIFT+F1 → mouse released"));
		}
		return true;
	}

private:
	static APlayerController* FindLocalPC()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if ((Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE) && Ctx.World())
			{
				if (APlayerController* PC = Ctx.World()->GetFirstPlayerController())
				{
					return PC;
				}
			}
		}
		return nullptr;
	}

	bool bCaptured = true;  // starts matching the game's default captured state
};

FDisplayXRCoreModule* FDisplayXRCoreModule::ModuleInstance = nullptr;

void FDisplayXRCoreModule::StartupModule()
{
	ModuleInstance = this;

#if PLATFORM_WINDOWS
	// Request per-monitor DPI awareness so the backbuffer uses physical pixels
	// (e.g., 3840x2160) instead of DPI-scaled logical pixels (e.g., 1536x864).
	// Must happen before rendering startup (PostConfigInit is early enough).
	SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Set DPI awareness to per-monitor"));
#endif

	// Under the shell (DISPLAYXR_WORKSPACE_SESSION) UE's window is never the OS
	// foreground window, so UE idles its render + STARTUP when unfocused — the app
	// comes up stuck on the loading spinner until you alt-tab in. Disable the
	// not-foreground idle here at module load (PostConfigInit — before the engine
	// can idle), not in the compositor's late xrCreateSession path. Code-set via
	// IConsoleManager because t.IdleWhenNotForeground is an ECVF_Cheat cvar the
	// engine refuses to read from DefaultEngine.ini.
	if (!FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAYXR_WORKSPACE_SESSION")).IsEmpty())
	{
		if (IConsoleVariable* CVarIdle = IConsoleManager::Get().FindConsoleVariable(TEXT("t.IdleWhenNotForeground")))
		{
			CVarIdle->Set(0, ECVF_SetByCode);
			UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: t.IdleWhenNotForeground=0 (shell: render while unfocused)"));
		}
#if PLATFORM_WINDOWS
		// Capture the shell's foreground window NOW (module load, before UE creates
		// and shows its game window and steals foreground). The compositor hands it
		// back after hiding UE's window so the shell keeps displaying the app
		// without the user having to alt-tab back.
		FDisplayXRPlatform::SavedShellForegroundHWND = (void*)::GetForegroundWindow();
		// Start the window-hider watcher NOW (before UE creates + shows its game
		// window), so UE's black window is clipped the instant it appears even while
		// the game thread is blocked loading — it otherwise covers the shell and
		// other apps for seconds.
		GWindowHider = new FDisplayXRWindowHider(FDisplayXRPlatform::SavedShellForegroundHWND);
		GWindowHiderThread = FRunnableThread::Create(GWindowHider, TEXT("DisplayXRWindowHider"));
#endif
	}

	// Set HMD priority higher than OpenXR/SteamVR so UE picks us
	float HighestXrPluginPriority = 0.0f;
	TArray<FString> XrPlugins = { TEXT("OpenXRHMD"), TEXT("SteamVR") };
	for (const FString& XrPlugin : XrPlugins)
	{
		float PluginPriority = 0.0f;
		GConfig->GetFloat(TEXT("HMDPluginPriority"), *XrPlugin, PluginPriority, GEngineIni);
		HighestXrPluginPriority = FMath::Max(PluginPriority, HighestXrPluginPriority);
	}
	GConfig->SetFloat(TEXT("HMDPluginPriority"), TEXT("DisplayXRCore"),
		HighestXrPluginPriority + 10, GEngineIni);

	// Create session (loads DisplayXR OpenXR runtime directly)
	Session = MakeShared<FDisplayXRSession>();
	if (!Session->Initialize())
	{
		UE_LOG(LogDisplayXRCore, Warning, TEXT("DisplayXR: Failed to initialize session"));
	}
	else
	{
		UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Session initialized"));
	}

	// Register with UE's HMD module discovery
	IHeadMountedDisplayModule::StartupModule();

	// Register SHIFT+F1 (mouse cursor toggle) and 'I' (atlas capture) Slate
	// preprocessor. The runtime module loads at PostConfigInit — earlier than
	// Slate — so the synchronous path almost always misses the window. Defer to
	// OnPostEngineInit, which fires after Slate is up in both editor and game
	// targets. Keep the synchronous path for the rare case where Slate is
	// somehow already up by the time we run (e.g. hot-reload).
	if (FSlateApplication::IsInitialized())
	{
		RegisterDevInputProcessor();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FDisplayXRCoreModule::RegisterDevInputProcessor);
	}

	AtlasCaptureCmd = MakeUnique<FAutoConsoleCommand>(
		TEXT("DisplayXR.CaptureAtlas"),
		TEXT("Capture the current swapchain atlas to %USERPROFILE%\\Pictures\\DisplayXR\\<ProjectName>-<N>_<cols>x<rows>.png."),
		FConsoleCommandDelegate::CreateStatic(&FDisplayXRAtlasCapture::RequestCapture));

	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Core module started (custom HMD path)"));
}

void FDisplayXRCoreModule::RegisterDevInputProcessor()
{
	if (!FSlateApplication::IsInitialized() || DevInputProcessor.IsValid())
	{
		return;
	}
	DevInputProcessor = MakeShared<FDisplayXRDevInputProcessor>();
	FSlateApplication::Get().RegisterInputPreProcessor(DevInputProcessor);
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Slate input preprocessor registered (SHIFT+F1, I)"));
}

void FDisplayXRCoreModule::ShutdownModule()
{
#if PLATFORM_WINDOWS
	if (GWindowHiderThread)
	{
		GWindowHiderThread->Kill(true);  // Stop() + wait
		delete GWindowHiderThread;
		GWindowHiderThread = nullptr;
	}
	if (GWindowHider) { delete GWindowHider; GWindowHider = nullptr; }
#endif

	AtlasCaptureCmd.Reset();
	ReleaseWovenSurface();

	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	if (DevInputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(DevInputProcessor);
	}
	DevInputProcessor.Reset();

	if (Session.IsValid())
	{
		Session->Shutdown();
		Session.Reset();
	}

	ModuleInstance = nullptr;
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Core module shut down"));

	IHeadMountedDisplayModule::ShutdownModule();
}

bool FDisplayXRCoreModule::IsHMDConnected()
{
	return Session.IsValid() && Session->IsActive();
}

TSharedPtr<IXRTrackingSystem, ESPMode::ThreadSafe> FDisplayXRCoreModule::CreateTrackingSystem()
{
	if (!Session.IsValid() || !Session->IsActive())
	{
		UE_LOG(LogDisplayXRCore, Warning, TEXT("DisplayXR: Cannot create tracking system, session not active"));
		return nullptr;
	}

	auto Device = FSceneViewExtensions::NewExtension<FDisplayXRDevice>(Session.Get());
	UE_LOG(LogDisplayXRCore, Log, TEXT("DisplayXR: Tracking system created"));
	return Device;
}

void FDisplayXRCoreModule::GetModuleAliases(TArray<FString>& AliasesOut) const
{
	AliasesOut.Add(TEXT("DisplayXR"));
}

FDisplayXRSession* FDisplayXRCoreModule::GetSession()
{
	if (ModuleInstance && ModuleInstance->Session.IsValid())
	{
		return ModuleInstance->Session.Get();
	}
	return nullptr;
}

/**
 * Resolve the active XR system to our device, or null when DisplayXR is not it.
 *
 * The device is owned by the engine (CreateTrackingSystem hands it over), so we
 * reach it back through GEngine rather than keeping a second reference. Another
 * XR plugin may be active instead, hence the system-name check before casting.
 */
static FDisplayXRDevice* GetActiveDisplayXRDevice()
{
	if (!GEngine || !GEngine->XRSystem.IsValid())
	{
		return nullptr;
	}
	if (GEngine->XRSystem->GetSystemName() != FName(TEXT("DisplayXR")))
	{
		return nullptr;
	}
	return static_cast<FDisplayXRDevice*>(GEngine->XRSystem.Get());
}

void FDisplayXRCoreModule::NotifyPlaySessionStarting()
{
	check(IsInGameThread());
	if (FDisplayXRDevice* Device = GetActiveDisplayXRDevice())
	{
		Device->RearmCompositorCreation();
	}
}

void FDisplayXRCoreModule::NotifyPlaySessionEnded()
{
	check(IsInGameThread());
	if (FDisplayXRDevice* Device = GetActiveDisplayXRDevice())
	{
		Device->ShutdownCompositorForSessionEnd();
	}
}

bool FDisplayXRCoreModule::GetOrCreateWovenSurface(uint32 W, uint32 H,
	void*& OutSharedHandle, FTextureRHIRef& OutTextureRHI)
{
#if PLATFORM_WINDOWS
	check(IsInGameThread());
	if (!ModuleInstance || !W || !H)
	{
		return false;
	}
	FDisplayXRCoreModule& M = *ModuleInstance;

	// Cache hit: same worst-case size (the normal case — it never changes for
	// a given display). A shared committed resource + NT handle + RHI wrap
	// costs ~1.5 s; paying it once per editor run instead of once per Play is
	// most of the restart smoothing.
	if (M.WovenTextureRHI.IsValid() && M.WovenSharedHandle && M.WovenW == W && M.WovenH == H)
	{
		OutSharedHandle = M.WovenSharedHandle;
		OutTextureRHI = M.WovenTextureRHI;
		return true;
	}
	M.ReleaseWovenSurface();

	ID3D12Device* Dev = GDynamicRHI ? static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice()) : nullptr;
	if (!Dev)
	{
		return false;
	}

	D3D12_RESOURCE_DESC RD = {};
	RD.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	RD.Width = W;
	RD.Height = H;
	RD.DepthOrArraySize = 1;
	RD.MipLevels = 1;
	RD.SampleDesc.Count = 1;
	// BGRA to match the reference apps; the runtime builds its RTV from our
	// GetDesc().Format. The presenter's blit must stay a format-converting
	// shader draw — a copy cannot cross the RGBA/BGRA copy groups.
	RD.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	RD.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// ALLOW_SIMULTANEOUS_ACCESS: the runtime's queue weaves into this while
	// UE's graphics queue samples it (accepted single-frame tear, Unity parity).
	RD.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

	D3D12_HEAP_PROPERTIES HP = {};
	HP.Type = D3D12_HEAP_TYPE_DEFAULT;

	ID3D12Resource* Res = nullptr;
	HRESULT hr = Dev->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_SHARED, &RD,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Res));
	if (FAILED(hr) || !Res)
	{
		UE_LOG(LogDisplayXRCore, Error, TEXT("DisplayXR: woven surface CreateCommittedResource failed (0x%08x)"), (uint32)hr);
		return false;
	}
	Res->SetName(L"DisplayXR.WovenPreview");

	HANDLE SharedHandle = nullptr;
	hr = Dev->CreateSharedHandle(Res, nullptr, GENERIC_ALL, nullptr, &SharedHandle);
	if (FAILED(hr) || !SharedHandle)
	{
		UE_LOG(LogDisplayXRCore, Error, TEXT("DisplayXR: woven surface CreateSharedHandle failed (0x%08x)"), (uint32)hr);
		Res->Release();
		return false;
	}

	ID3D12DynamicRHI* DynamicRHI = GetID3D12DynamicRHI();
	FTextureRHIRef* OutRef = &M.WovenTextureRHI;
	ENQUEUE_RENDER_COMMAND(WrapDisplayXRWovenSurface)(
		[OutRef, Res, DynamicRHI](FRHICommandListImmediate&)
		{
			*OutRef = DynamicRHI->RHICreateTexture2DFromResource(
				PF_B8G8R8A8, ETextureCreateFlags::ShaderResource,
				FClearValueBinding::Black, Res);
		});
	FlushRenderingCommands();

	if (!M.WovenTextureRHI.IsValid())
	{
		::CloseHandle(SharedHandle);
		Res->Release();
		return false;
	}

	M.WovenResource = Res;
	M.WovenSharedHandle = SharedHandle;
	M.WovenW = W;
	M.WovenH = H;
	UE_LOG(LogDisplayXRCore, Log,
		TEXT("DisplayXR: woven surface %ux%u BGRA created (process-lifetime, reused across play sessions) handle=%p"),
		W, H, SharedHandle);

	OutSharedHandle = M.WovenSharedHandle;
	OutTextureRHI = M.WovenTextureRHI;
	return true;
#else
	return false;
#endif
}

void FDisplayXRCoreModule::ReleaseWovenSurface()
{
#if PLATFORM_WINDOWS
	if (WovenTextureRHI.IsValid())
	{
		FlushRenderingCommands();
		WovenTextureRHI.SafeRelease();
	}
	if (WovenSharedHandle) { ::CloseHandle((HANDLE)WovenSharedHandle); WovenSharedHandle = nullptr; }
	if (WovenResource) { static_cast<ID3D12Resource*>(WovenResource)->Release(); WovenResource = nullptr; }
	WovenW = WovenH = 0;
#endif
}

FTextureRHIRef FDisplayXRCoreModule::GetWovenTextureRHI_GameThread()
{
	check(IsInGameThread());
	if (FDisplayXRDevice* Device = GetActiveDisplayXRDevice())
	{
		if (FDisplayXRCompositor* Compositor = Device->GetCompositor())
		{
			// Withhold the texture until the display processor has actually
			// woven a few frames into it: blitting a freshly created (black)
			// surface over the tab is the "editor goes black on restart"
			// symptom. Until then the tab keeps showing UE's own SBS render.
			if (Compositor->GetZonesFramesSubmitted() >= 3)
			{
				return Compositor->GetWovenTextureRHI();
			}
		}
	}
	return nullptr;
}

void* FDisplayXRPlatform::OverrideCompositorHWND = nullptr;
void* FDisplayXRPlatform::SavedShellForegroundHWND = nullptr;
bool FDisplayXRPlatform::bRequestSharedTextureBinding = false;
TAtomic<uint64> FDisplayXRPlatform::EditorZoneSizePacked{0};

IMPLEMENT_MODULE(FDisplayXRCoreModule, DisplayXRCore)
