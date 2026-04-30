// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRAtlasCapture.h"

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Math/IntRect.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RHICommandList.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRCapture, Log, All);

namespace DisplayXRAtlasCaptureNS
{
	static FThreadSafeCounter PendingRequests;

	static FString ResolveCaptureDir()
	{
		// %USERPROFILE%\Pictures\DisplayXR — parity with runtime-pvt test_apps.
		FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (UserProfile.IsEmpty())
		{
			// Fallback: project-local Saved dir.
			return FPaths::ProjectSavedDir() / TEXT("DisplayXR") / TEXT("Captures");
		}
		return UserProfile / TEXT("Pictures") / TEXT("DisplayXR");
	}

	static int32 NextSequenceNumber(const FString& Dir, const FString& Stem, int32 Cols, int32 Rows)
	{
		// Find files like "<Stem>-<N>_<Cols>x<Rows>.png" and return max(N)+1, starting at 1.
		const FString Suffix = FString::Printf(TEXT("_%dx%d.png"), Cols, Rows);
		const FString Wildcard = Stem + TEXT("-*") + Suffix;

		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Dir / Wildcard), /*Files=*/true, /*Dirs=*/false);

		int32 MaxN = 0;
		for (const FString& File : Found)
		{
			// Strip "<Stem>-" prefix and "_<Cols>x<Rows>.png" suffix, parse the middle.
			FString Tail = File;
			if (!Tail.StartsWith(Stem + TEXT("-"))) continue;
			Tail = Tail.Mid(Stem.Len() + 1);
			if (!Tail.EndsWith(Suffix)) continue;
			Tail = Tail.LeftChop(Suffix.Len());
			if (Tail.IsNumeric())
			{
				MaxN = FMath::Max(MaxN, FCString::Atoi(*Tail));
			}
		}
		return MaxN + 1;
	}

	static FString MakeOutputPath(int32 Cols, int32 Rows)
	{
		const FString Dir = ResolveCaptureDir();
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		const FString Stem = FString(FApp::GetProjectName());
		const int32 N = NextSequenceNumber(Dir, Stem, Cols, Rows);
		return Dir / FString::Printf(TEXT("%s-%d_%dx%d.png"), *Stem, N, Cols, Rows);
	}

#if PLATFORM_WINDOWS
	// Transient layered top-level window that fills white for ~80ms over the
	// active editor/game window, then auto-destroys via WM_TIMER. Equivalent to
	// the runtime-pvt reference flash, which paints inside the app's own
	// WindowProc — we can't intercept UE's WindowProc cleanly, so we overlay.
	static const TCHAR* kFlashClassName = TEXT("DisplayXRCaptureFlash");
	static const UINT_PTR kFlashTimerId = 1;
	static bool bFlashClassRegistered = false;

	static LRESULT CALLBACK FlashWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg)
		{
		case WM_PAINT:
			{
				PAINTSTRUCT ps;
				HDC hdc = BeginPaint(hwnd, &ps);
				HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
				FillRect(hdc, &ps.rcPaint, brush);
				DeleteObject(brush);
				EndPaint(hwnd, &ps);
			}
			return 0;
		case WM_TIMER:
			if (wp == kFlashTimerId)
			{
				KillTimer(hwnd, kFlashTimerId);
				DestroyWindow(hwnd);
			}
			return 0;
		case WM_DESTROY:
			return 0;
		default:
			return DefWindowProc(hwnd, msg, wp, lp);
		}
	}

	static void EnsureFlashClassRegistered()
	{
		if (bFlashClassRegistered) return;
		WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = &FlashWndProc;
		wc.hInstance = GetModuleHandle(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wc.lpszClassName = kFlashClassName;
		RegisterClassEx(&wc);
		bFlashClassRegistered = true;
	}

	static void TriggerFlashOverlay_GameThread()
	{
		check(IsInGameThread());

		HWND ParentHwnd = nullptr;
		if (FSlateApplication::IsInitialized())
		{
			TSharedPtr<SWindow> Active = FSlateApplication::Get().GetActiveTopLevelWindow();
			if (Active.IsValid())
			{
				TSharedPtr<FGenericWindow> Native = Active->GetNativeWindow();
				if (Native.IsValid())
				{
					ParentHwnd = static_cast<HWND>(Native->GetOSWindowHandle());
				}
			}
		}
		if (!ParentHwnd) return;

		RECT R;
		if (!GetWindowRect(ParentHwnd, &R)) return;

		EnsureFlashClassRegistered();

		HWND Flash = CreateWindowEx(
			WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
			kFlashClassName,
			TEXT(""),
			WS_POPUP,
			R.left, R.top, R.right - R.left, R.bottom - R.top,
			nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
		if (!Flash) return;

		// 70% alpha — visible flash without blinding.
		SetLayeredWindowAttributes(Flash, 0, 0xB3, LWA_ALPHA);
		ShowWindow(Flash, SW_SHOWNOACTIVATE);
		UpdateWindow(Flash);
		SetTimer(Flash, kFlashTimerId, /*ms=*/80, nullptr);
	}
#endif // PLATFORM_WINDOWS

	static bool EncodeAndSavePng(const TArray<FColor>& Pixels, int32 W, int32 H, const FString& OutPath)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid())
		{
			return false;
		}
		const int64 RawSize = (int64)W * H * sizeof(FColor);
		if (!Wrapper->SetRaw(Pixels.GetData(), RawSize, W, H, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8>& Compressed = Wrapper->GetCompressed(/*Quality=*/100);
		if (Compressed.Num() == 0)
		{
			return false;
		}
		return FFileHelper::SaveArrayToFile(Compressed, *OutPath);
	}
}

void FDisplayXRAtlasCapture::RequestCapture()
{
	const int32 New = DisplayXRAtlasCaptureNS::PendingRequests.Increment();
	UE_LOG(LogDisplayXRCapture, Log, TEXT("Atlas capture armed (pending=%d)"), New);
}

void FDisplayXRAtlasCapture::ProcessRequest_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FRHITexture* AtlasSrc,
	int32 AtlasW,
	int32 AtlasH,
	int32 Cols,
	int32 Rows)
{
	using namespace DisplayXRAtlasCaptureNS;

	if (PendingRequests.GetValue() <= 0) return;
	if (!AtlasSrc || AtlasW <= 0 || AtlasH <= 0 || Cols <= 0 || Rows <= 0)
	{
		// Drain the request even if we can't service it — avoids a sticky arm.
		PendingRequests.Decrement();
		UE_LOG(LogDisplayXRCapture, Warning, TEXT("Atlas capture skipped: bad inputs (Atlas=%dx%d Tiles=%dx%d)"), AtlasW, AtlasH, Cols, Rows);
		return;
	}

	const FIntRect Rect(0, 0, AtlasW, AtlasH);
	TArray<FColor> Pixels;
	RHICmdList.ReadSurfaceData(AtlasSrc, Rect, Pixels, FReadSurfaceDataFlags());

	if (Pixels.Num() < AtlasW * AtlasH)
	{
		PendingRequests.Decrement();
		UE_LOG(LogDisplayXRCapture, Warning, TEXT("Atlas capture: ReadSurfaceData returned %d pixels, expected %d"), Pixels.Num(), AtlasW * AtlasH);
		return;
	}

	const FString OutPath = MakeOutputPath(Cols, Rows);
	const bool bSaved = EncodeAndSavePng(Pixels, AtlasW, AtlasH, OutPath);
	PendingRequests.Decrement();

	if (bSaved)
	{
		UE_LOG(LogDisplayXRCapture, Log, TEXT("Atlas captured: %s (%dx%d, %dx%d tiles)"), *OutPath, AtlasW, AtlasH, Cols, Rows);
#if PLATFORM_WINDOWS
		AsyncTask(ENamedThreads::GameThread, []()
		{
			TriggerFlashOverlay_GameThread();
		});
#endif
	}
	else
	{
		UE_LOG(LogDisplayXRCapture, Error, TEXT("Atlas capture: failed to write PNG to %s"), *OutPath);
	}
}
