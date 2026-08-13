// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRPIEPreview.h"

#if PLATFORM_WINDOWS

#include "DisplayXRPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "XRCopyTexture.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRPIEPreview, Log, All);

// =============================================================================
// Proxy window: phase anchor only. Invisible (layered alpha 0), click-through
// (HTTRANSPARENT), never activated, never presented to.
// =============================================================================

static const wchar_t* PROXY_CLASS = L"DisplayXRPIEProxy";
static bool bProxyClassRegistered = false;

static LRESULT CALLBACK ProxyWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_NCHITTEST:
		// All mouse goes to the PIE viewport rendered beneath us.
		return HTTRANSPARENT;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_CLOSE:
		return 0;
	}
	// Runtime-forwarded input (the weaver PostMessages to the bound HWND) is
	// deliberately dropped here: in-editor, real input already reaches the
	// viewport directly — unlike the game-mode overlay, nothing needs relaying.
	return DefWindowProcW(h, m, w, l);
}

static bool RegisterProxyClass()
{
	if (bProxyClassRegistered)
	{
		return true;
	}
	WNDCLASSW wc = {};
	wc.lpfnWndProc = ProxyWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = PROXY_CLASS;
	wc.hbrBackground = nullptr;
	bProxyClassRegistered = RegisterClassW(&wc) != 0;
	return bProxyClassRegistered;
}

static uint64 PackRect(int32 X, int32 Y, int32 W, int32 H)
{
	return ((uint64)(uint16)X << 48) | ((uint64)(uint16)Y << 32)
	     | ((uint64)(uint16)W << 16) | (uint64)(uint16)H;
}

static void UnpackRect(uint64 Packed, int32& X, int32& Y, int32& W, int32& H)
{
	X = (int32)(int16)((Packed >> 48) & 0xffff);
	Y = (int32)(int16)((Packed >> 32) & 0xffff);
	W = (int32)(uint16)((Packed >> 16) & 0xffff);
	H = (int32)(uint16)(Packed & 0xffff);
}

// =============================================================================
// FDisplayXRPIEPreview
// =============================================================================

FDisplayXRPIEPreview::~FDisplayXRPIEPreview()
{
	Stop();
}

bool FDisplayXRPIEPreview::TryStart(TSharedRef<SViewport> ViewportWidget)
{
	check(IsInGameThread());
	if (ProxyHWND)
	{
		return true;
	}

	// Slate must have arranged the widget: we need real geometry and the
	// owning top-level window. Both are unavailable the frame PIE starts.
	const FGeometry& Geo = ViewportWidget->GetCachedGeometry();
	const FVector2D AbsPos = Geo.GetAbsolutePosition();
	const FVector2D AbsSize = Geo.GetAbsoluteSize();
	if (AbsSize.X < 64.0 || AbsSize.Y < 64.0)
	{
		return false;
	}

	TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(ViewportWidget);
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
	{
		return false;
	}
	HWND ParentHWND = (HWND)Window->GetNativeWindow()->GetOSWindowHandle();
	if (!ParentHWND)
	{
		return false;
	}

	// Slate's desktop space is physical (virtual-screen) pixels on Windows, so
	// the absolute geometry maps to parent client coords via ScreenToClient.
	// Logged below against GetClientRect so a DPI mismatch is diagnosable.
	POINT TL = {FMath::RoundToInt((float)AbsPos.X), FMath::RoundToInt((float)AbsPos.Y)};
	::ScreenToClient(ParentHWND, &TL);
	// EVEN dims: the tile math halves this rect on three sides (UE's view
	// rects, the atlas copy, the runtime's imageRects) — even dims make the
	// halving exact everywhere, so no 1px eye bleed at the tile seam.
	const int32 W = FMath::RoundToInt((float)AbsSize.X) & ~1;
	const int32 H = FMath::RoundToInt((float)AbsSize.Y) & ~1;

	if (!RegisterProxyClass())
	{
		UE_LOG(LogDisplayXRPIEPreview, Error, TEXT("Proxy window class registration failed"));
		return false;
	}

	// NEVER SetWindowPos with SWP_FRAMECHANGED on this HWND once the weaver is
	// bound, and never restyle it — that collapses stereo to mono permanently.
	// Plain move/resize only (M3).
	ProxyHWND = CreateWindowExW(
		WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
		PROXY_CLASS, L"DisplayXR PIE Proxy",
		WS_CHILD | WS_VISIBLE,
		TL.x, TL.y, W, H,
		ParentHWND, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!ProxyHWND)
	{
		UE_LOG(LogDisplayXRPIEPreview, Error, TEXT("Proxy window creation failed (err=%u)"), ::GetLastError());
		return false;
	}
	// Fully invisible. WS_VISIBLE + alpha 0: the window must be "shown" for the
	// display processor to resolve its screen rect, but no pixel of it ever
	// reaches the screen.
	::SetLayeredWindowAttributes((HWND)ProxyHWND, 0, 0, LWA_ALPHA);

	// Publish BEFORE the compositor can be created (it latches these at
	// Initialize): bound HWND = proxy, canvas = full proxy.
	// bRequestSharedTextureBinding is already set by the editor module at
	// OnPostPIEStarted — it must predate the first stereo frame, not just the
	// compositor (it keeps Slate off the stereo-composite path).
	FDisplayXRPlatform::OverrideCompositorHWND = ProxyHWND;
	FDisplayXRPlatform::SetEditorZoneSize((uint32)W, (uint32)H);

	BlitRectPacked.Store(PackRect(TL.x, TL.y, W, H));
	TargetWindowPtr = Window.Get();
	ViewportWidgetWeak = ViewportWidget;
	ClientX = TL.x; ClientY = TL.y; ClientW = W; ClientH = H;
	bResizePending = false;

	// Take the PIE viewport OUT of the window's registered-viewport slot (see
	// the header note — no restore, deliberately).
	{
		TSharedPtr<ISlateViewport> WindowViewport = Window->GetViewport();
		TSharedPtr<ISlateViewport> WidgetViewport = ViewportWidget->GetViewportInterface().Pin();
		if (WindowViewport.IsValid() && WindowViewport == WidgetViewport)
		{
			Window->UnsetViewport(WindowViewport.ToSharedRef());
			UE_LOG(LogDisplayXRPIEPreview, Log,
				TEXT("Unset the PIE viewport from the window's composite slot (widget-drawn for the preview)"));
		}
	}

	BackBufferReadyHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent()
		.AddRaw(this, &FDisplayXRPIEPreview::OnBackBufferReady_RenderThread);

	// The woven texture appears once the compositor initializes (deferred to
	// the first viewport draw). Poll on the game thread and hand the ref to
	// the render thread exactly once.
	WovenPollTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[this](float) -> bool
		{
			FTextureRHIRef Woven = FDisplayXRCoreModule::GetWovenTextureRHI_GameThread();
			if (!Woven.IsValid())
			{
				return true; // keep polling
			}
			FDisplayXRPIEPreview* Self = this;
			ENQUEUE_RENDER_COMMAND(SetDisplayXRWovenTexture)(
				[Self, Woven](FRHICommandListImmediate&)
				{
					Self->WovenTexture_RT = Woven;
				});
			UE_LOG(LogDisplayXRPIEPreview, Log, TEXT("Woven texture published to the presenter (%ux%u)"),
				Woven->GetSizeX(), Woven->GetSizeY());
			WovenPollTicker.Reset();
			return false;
		}));

	// M3 live glue: track the viewport every editor tick while active.
	UpdateTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FDisplayXRPIEPreview::TickUpdate));

	RECT ProxyClient = {};
	::GetClientRect((HWND)ProxyHWND, &ProxyClient);
	UE_LOG(LogDisplayXRPIEPreview, Log,
		TEXT("Texture-mode preview started: proxy=%p parent=%p rect=(%d,%d %dx%d) proxyClient=%dx%d window='%s'"),
		ProxyHWND, (void*)ParentHWND, TL.x, TL.y, W, H,
		(int)ProxyClient.right, (int)ProxyClient.bottom, *Window->GetTitle().ToString());
	return true;
}

bool FDisplayXRPIEPreview::TickUpdate(float /*DeltaTime*/)
{
	check(IsInGameThread());
	if (!ProxyHWND)
	{
		return false;
	}

	TSharedPtr<SViewport> Widget = ViewportWidgetWeak.Pin();
	if (!Widget.IsValid())
	{
		return true; // PIE is ending; module teardown owns cleanup
	}

	// Dock/float moved the viewport into a different top-level window: the
	// proxy's parent, the blit target window, and the window-viewport-slot
	// dance are all wrong now. Full restart — teardown keeps the texture-mode
	// flag so UE never touches the separate-RT path in between, and TryStart
	// rebinds against the new window (session rebind included). SetParent on
	// the weaver-bound HWND is deliberately avoided: restyling/reparenting a
	// bound window is the known stereo-collapse trap.
	TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
	{
		return true;
	}
	if (Window.Get() != TargetWindowPtr)
	{
		UE_LOG(LogDisplayXRPIEPreview, Log,
			TEXT("Viewport moved to another window ('%s') — restarting the preview against it"),
			*Window->GetTitle().ToString());
		Stop(/*bClearModeFlag=*/false);
		if (TryStart(Widget.ToSharedRef()))
		{
			FDisplayXRCoreModule::NotifyPlaySessionStarting();
		}
		return false; // TryStart registered a fresh ticker
	}

	HWND ParentHWND = (HWND)Window->GetNativeWindow()->GetOSWindowHandle();
	if (!ParentHWND)
	{
		return true;
	}

	const FGeometry& Geo = Widget->GetCachedGeometry();
	const FVector2D AbsPos = Geo.GetAbsolutePosition();
	const FVector2D AbsSize = Geo.GetAbsoluteSize();
	if (AbsSize.X < 64.0 || AbsSize.Y < 64.0)
	{
		return true; // mid-layout; keep the last good rect
	}

	POINT TL = {FMath::RoundToInt((float)AbsPos.X), FMath::RoundToInt((float)AbsPos.Y)};
	::ScreenToClient(ParentHWND, &TL);
	const int32 NewW = FMath::RoundToInt((float)AbsSize.X) & ~1;
	const int32 NewH = FMath::RoundToInt((float)AbsSize.Y) & ~1;

	// Moves apply immediately: interlace phase is computed from the proxy's
	// live screen rect, so a stale position weaves for the wrong subpixels.
	// (Whole-window drags don't land here — the proxy is a child, so its
	// client-relative rect is unchanged and the DP tracks the screen move via
	// the HWND on its own. This catches layout changes: splitters, tabs.)
	if (TL.x != ClientX || TL.y != ClientY)
	{
		ClientX = TL.x;
		ClientY = TL.y;
		::SetWindowPos((HWND)ProxyHWND, nullptr, ClientX, ClientY, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		BlitRectPacked.Store(PackRect(ClientX, ClientY,
			FMath::Min(NewW, ClientW), FMath::Min(NewH, ClientH)));
		UE_LOG(LogDisplayXRPIEPreview, Log, TEXT("Preview moved: proxy -> (%d,%d)"), ClientX, ClientY);
	}

	// Sizes settle-debounce (per-frame canvas resize = runtime swapchain-
	// realloc storms). During the drag, keep the old canvas but never paint
	// outside the shrunk widget: clamp the blit to min(canvas, widget).
	if (NewW != ClientW || NewH != ClientH)
	{
		if (!bResizePending || NewW != PendingW || NewH != PendingH)
		{
			bResizePending = true;
			PendingW = NewW;
			PendingH = NewH;
			LastResizeChangeTime = FPlatformTime::Seconds();
			BlitRectPacked.Store(PackRect(ClientX, ClientY,
				FMath::Min(NewW, ClientW), FMath::Min(NewH, ClientH)));
		}
		else if (FPlatformTime::Seconds() - LastResizeChangeTime >= ResizeSettleSeconds)
		{
			bResizePending = false;
			ClientW = PendingW;
			ClientH = PendingH;
			// Plain resize only — never SWP_FRAMECHANGED on the weaver-bound
			// HWND (permanent mono-collapse trap).
			::SetWindowPos((HWND)ProxyHWND, nullptr, 0, 0, ClientW, ClientH,
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			FDisplayXRPlatform::SetEditorZoneSize((uint32)ClientW, (uint32)ClientH);
			BlitRectPacked.Store(PackRect(ClientX, ClientY, ClientW, ClientH));
			UE_LOG(LogDisplayXRPIEPreview, Log,
				TEXT("Resize settled: canvas -> %dx%d (zone + proxy + blit updated)"), ClientW, ClientH);
		}
	}
	else if (bResizePending)
	{
		// Bounced back to the current size before settling.
		bResizePending = false;
		BlitRectPacked.Store(PackRect(ClientX, ClientY, ClientW, ClientH));
	}

	return true;
}

void FDisplayXRPIEPreview::Stop(bool bClearModeFlag)
{
	check(IsInGameThread());

	if (UpdateTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(UpdateTicker);
		UpdateTicker.Reset();
	}
	if (WovenPollTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(WovenPollTicker);
		WovenPollTicker.Reset();
	}

	const bool bSlateAlive = FSlateApplication::IsInitialized();
	if (BackBufferReadyHandle.IsValid())
	{
		if (bSlateAlive)
		{
			FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferReadyHandle);
		}
		BackBufferReadyHandle.Reset();
	}

	// Drop the render thread's ref and drain in-flight blits BEFORE the
	// compositor teardown releases the woven texture underneath them.
	if (WovenTexture_RT.IsValid() || TargetWindowPtr)
	{
		FDisplayXRPIEPreview* Self = this;
		ENQUEUE_RENDER_COMMAND(ClearDisplayXRWovenTexture)(
			[Self](FRHICommandListImmediate&)
			{
				Self->WovenTexture_RT.SafeRelease();
			});
		FlushRenderingCommands();
	}
	TargetWindowPtr = nullptr;

	if (ProxyHWND)
	{
		// Compositor down while the proxy — the HWND its session is bound to —
		// is still alive. Leaves compositor creation disarmed for the rest of
		// teardown, so nothing rebinds to the window we're about to destroy.
		FDisplayXRCoreModule::NotifyPlaySessionEnded();
	}

	// On a mid-PIE restart the texture-mode flag stays set: UE must never
	// touch the separate-RT path between Stop and the re-TryStart.
	if (bClearModeFlag)
	{
		FDisplayXRPlatform::bRequestSharedTextureBinding = false;
	}
	FDisplayXRPlatform::SetEditorZoneSize(0, 0);
	FDisplayXRPlatform::OverrideCompositorHWND = nullptr;
	BlitRectPacked.Store(0);
	bResizePending = false;

	if (ProxyHWND)
	{
		::DestroyWindow((HWND)ProxyHWND);
		ProxyHWND = nullptr;
		UE_LOG(LogDisplayXRPIEPreview, Log, TEXT("Texture-mode preview stopped; proxy destroyed"));
	}
}

void FDisplayXRPIEPreview::OnBackBufferReady_RenderThread(SWindow& Window, const FTextureRHIRef& BackBuffer)
{
	if (&Window != TargetWindowPtr || !WovenTexture_RT.IsValid() || !BackBuffer.IsValid())
	{
		return;
	}

	int32 X, Y, W, H;
	UnpackRect(BlitRectPacked.Load(), X, Y, W, H);
	if (W <= 0 || H <= 0)
	{
		return;
	}

	// Clamp on our side: the runtime validates no dimensions (spec), and during
	// a resize the rect can be stale for a frame — skip rather than smear.
	const FIntPoint BBSize(BackBuffer->GetSizeX(), BackBuffer->GetSizeY());
	if (X < 0 || Y < 0 || X + W > BBSize.X || Y + H > BBSize.Y)
	{
		return;
	}
	const int32 SrcW = FMath::Min(W, (int32)WovenTexture_RT->GetSizeX());
	const int32 SrcH = FMath::Min(H, (int32)WovenTexture_RT->GetSizeY());

	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(RHICmdList);
	FRDGTextureRef Src = RegisterExternalTexture(GraphBuilder, WovenTexture_RT.GetReference(), TEXT("DisplayXRWoven"));
	FRDGTextureRef Dst = RegisterExternalTexture(GraphBuilder, BackBuffer.GetReference(), TEXT("DisplayXREditorBackBuffer"));

	FXRCopyTextureOptions Options(GMaxRHIFeatureLevel);
	Options.LoadAction = ERenderTargetLoadAction::ELoad;
	Options.BlendMod = EXRCopyTextureBlendModifier::Opaque;

	// 1:1 read-back from the origin: post-ADR-031 the canvas is origin-anchored
	// at the window client size and the rest of the worst-case surface is
	// UNDEFINED. Equal src/dst extents select point sampling inside the pass —
	// subpixel-exact, which the interlaced image requires (any scaling destroys
	// the weave).
	AddXRCopyTexturePass(GraphBuilder, RDG_EVENT_NAME("DisplayXRWovenBlit"),
		Src, FIntRect(0, 0, SrcW, SrcH),
		Dst, FIntRect(X, Y, X + SrcW, Y + SrcH),
		Options);
	GraphBuilder.Execute();

	static int32 BlitCount = 0;
	++BlitCount;
	if (BlitCount == 1 || BlitCount % 300 == 0)
	{
		UE_LOG(LogDisplayXRPIEPreview, Log,
			TEXT("Woven blit %s (#%d): dst=(%d,%d %dx%d) backbuffer=%dx%d fmt=%d src=%ux%u"),
			BlitCount == 1 ? TEXT("ACTIVE") : TEXT("alive"), BlitCount,
			X, Y, SrcW, SrcH, BBSize.X, BBSize.Y, (int)BackBuffer->GetFormat(),
			WovenTexture_RT->GetSizeX(), WovenTexture_RT->GetSizeY());
	}
	static bool bLoggedBlit = false;
	if (!bLoggedBlit)
	{
		bLoggedBlit = true;
		if (BackBuffer->GetFormat() != PF_B8G8R8A8 && BackBuffer->GetFormat() != PF_R8G8B8A8)
		{
			UE_LOG(LogDisplayXRPIEPreview, Warning,
				TEXT("Editor backbuffer is not 8-bit RGBA/BGRA (fmt=%d, likely HDR) — the weave geometry ")
				TEXT("is intact but colors will be off. Run the editor in SDR for a correct preview."),
				(int)BackBuffer->GetFormat());
		}
	}
}

#endif // PLATFORM_WINDOWS
