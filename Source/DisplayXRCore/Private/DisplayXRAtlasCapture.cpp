// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRAtlasCapture.h"

#include "DisplayXRCoreModule.h"
#include "DisplayXRSession.h"

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRCapture, Log, All);

namespace DisplayXRAtlasCaptureNS
{
	static FString ResolveCaptureDir()
	{
		// %USERPROFILE%\Pictures\DisplayXR — parity with the native test apps/demos.
		FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (UserProfile.IsEmpty())
		{
			// Fallback: project-local Saved dir.
			return FPaths::ProjectSavedDir() / TEXT("DisplayXR") / TEXT("Captures");
		}
		return UserProfile / TEXT("Pictures") / TEXT("DisplayXR");
	}

	// Path PREFIX (bare — no layout tokens, no extension) for xrCaptureAtlasDXR.
	// The runtime owns the suffix and appends "_atlas_<viewCount>_<cols>x<rows>.png"
	// (see DisplayXR/displayxr-runtime#425). The prefix must NOT repeat the layout
	// or the final name duplicates it (..._2x1_atlas_2_2x1.png). Numbers against
	// existing "<Stem>-<N>_atlas_*.png" files so repeat captures accumulate
	// instead of overwriting.
	static FString MakeOutputPrefix()
	{
		const FString Dir = ResolveCaptureDir();
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		const FString Stem = FString(FApp::GetProjectName());

		// Runtime-produced names look like "<Stem>-<N>_atlas_<views>_<c>x<r>.png".
		const FString Prefix = Stem + TEXT("-");
		const FString Wildcard = Prefix + TEXT("*_atlas_*.png");
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Dir / Wildcard), /*Files=*/true, /*Dirs=*/false);

		int32 MaxN = 0;
		for (const FString& File : Found)
		{
			if (!File.StartsWith(Prefix)) continue;
			FString Tail = File.Mid(Prefix.Len()); // "<N>_atlas_<views>_<c>x<r>.png"
			int32 Underscore = INDEX_NONE;
			if (!Tail.FindChar(TEXT('_'), Underscore)) continue; // first '_' ends <N>
			const FString NumStr = Tail.Left(Underscore);
			if (NumStr.IsNumeric())
			{
				MaxN = FMath::Max(MaxN, FCString::Atoi(*NumStr));
			}
		}
		// "<Dir>/<Stem>-<N>" — runtime appends "_atlas_<viewCount>_<cols>x<rows>.png".
		return Dir / FString::Printf(TEXT("%s-%d"), *Stem, MaxN + 1);
	}

#if PLATFORM_WINDOWS
	// Transient layered top-level window that fills white for ~80ms over the
	// active editor/game window, then auto-destroys via WM_TIMER. The runtime
	// capture is silent, so the affordance stays app-side (we can't intercept
	// UE's WindowProc cleanly, so we overlay).
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

	static void DoCapture_GameThread()
	{
		check(IsInGameThread());

		FDisplayXRSession* Session = FDisplayXRCoreModule::GetSession();
		if (!Session)
		{
			UE_LOG(LogDisplayXRCapture, Warning, TEXT("Atlas capture skipped: no active session"));
			return;
		}

		const FDisplayXRViewConfig View = Session->GetViewConfig();
		const int32 Cols = FMath::Max(View.TileColumns, 1);
		const int32 Rows = FMath::Max(View.TileRows, 1);
		if (Cols <= 1 && Rows <= 1)
		{
			UE_LOG(LogDisplayXRCapture, Warning, TEXT("Atlas capture skipped: mono (1x1) layout"));
			return;
		}

		const FString Prefix = MakeOutputPrefix();
		// PROJECTION_ONLY: the app's projection atlas (no runtime chrome), parity
		// with the native test apps. The runtime appends
		// "_atlas_<viewCount>_<cols>x<rows>.png" (DisplayXR/displayxr-runtime#425).
		const bool bOk = Session->CaptureAtlas(Prefix, /*bProjectionOnly=*/true);
		if (bOk)
		{
			UE_LOG(LogDisplayXRCapture, Log, TEXT("Atlas capture requested -> %s_atlas_%d_%dx%d.png"),
				*Prefix, View.GetViewCount(), Cols, Rows);
#if PLATFORM_WINDOWS
			TriggerFlashOverlay_GameThread();
#endif
		}
	}
}

void FDisplayXRAtlasCapture::RequestCapture()
{
	using namespace DisplayXRAtlasCaptureNS;
	if (IsInGameThread())
	{
		DoCapture_GameThread();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, []() { DoCapture_GameThread(); });
	}
}
