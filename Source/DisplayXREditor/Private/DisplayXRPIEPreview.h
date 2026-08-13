// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS

#include "RHIResources.h"
#include "Containers/Ticker.h"
#include "Templates/Atomic.h"

class SViewport;
class SWindow;

/**
 * Weave-to-texture PIE preview (#38): shows the runtime's WOVEN output inside
 * the PIE viewport tab, Unity-parity style. Three cooperating pieces:
 *
 *  1. An invisible, click-through proxy child window glued over the PIE
 *     viewport. It never presents — it exists because the display processor
 *     needs a real HWND to track screen position for interlace phase (a zone
 *     rect alone is not enough; the weaver anchors to the window association).
 *  2. Platform flags telling the compositor to bind the session in TEXTURE
 *     mode (shared surface + this proxy as the bound HWND) and publishing the
 *     full-window zone size that becomes the weave canvas.
 *  3. A render-thread hook on FSlateRenderer::OnBackBufferReadyToPresent that
 *     blits the woven texture 1:1 into the backbuffer region under the
 *     viewport — after Slate drew the pre-weave atlas there, so the overwrite
 *     IS the preview, and no hook means the tab falls back to the atlas.
 *
 * M2 scope: the rect is captured once at PIE start. Live glue (move/resize/
 * dock changes) is M3.
 */
class FDisplayXRPIEPreview
{
public:
	~FDisplayXRPIEPreview();

	/**
	 * Try to start the preview over the given PIE viewport widget. Returns
	 * false when Slate hasn't arranged the widget yet (no geometry, no owning
	 * window) — call again next tick. Idempotent once started.
	 */
	bool TryStart(TSharedRef<SViewport> ViewportWidget);

	/**
	 * Full teardown in dependency order: blit hook off (+ render flush), then
	 * the compositor (via NotifyPlaySessionEnded, while the proxy its session
	 * is bound to is still alive), then platform flags, then the proxy window.
	 */
	void Stop();

	bool IsActive() const { return ProxyHWND != nullptr; }

private:
	void OnBackBufferReady_RenderThread(SWindow& Window, const FTextureRHIRef& BackBuffer);

	void* ProxyHWND = nullptr;

	/** Identity only — compared, never dereferenced, on the render thread.
	 *  Written before the hook registers, cleared after it unregisters + a
	 *  render flush, so the hook can't observe a stale value. */
	SWindow* TargetWindowPtr = nullptr;

	/** RegisterGameViewport installs the PIE viewport into the owning window's
	 *  viewport slot at Play. A registered stereo viewport flips Slate's
	 *  window present onto the stereo-composite path (whole editor UI rendered
	 *  into the XR render target, presents starved behind our no-op composite
	 *  hook). We UNSET it for the duration — the widget still paints its
	 *  viewport as an ordinary quad, the pre-PIE arrangement — and restore it
	 *  in Stop() before PIE teardown's own UnsetViewport runs (its ensure
	 *  expects the slot to match). */
	TWeakPtr<SWindow> TargetWindowWeak;
	TWeakPtr<class ISlateViewport> RemovedWindowViewport;

	FDelegateHandle BackBufferReadyHandle;
	FTSTicker::FDelegateHandle WovenPollTicker;

	/** Blit rect in target-window client px, packed 16 bits per component
	 *  (x << 48 | y << 32 | w << 16 | h). Game thread writes, render thread
	 *  reads. */
	TAtomic<uint64> BlitRectPacked{0};

	/** Render-thread-owned; set/cleared via render commands from the game
	 *  thread (Stop clears it and flushes before the compositor releases the
	 *  underlying texture). */
	FTextureRHIRef WovenTexture_RT;
};

#else // !PLATFORM_WINDOWS

class SViewport;

/** Non-Windows stub: TryStart never succeeds, so the module's start ticker
 *  times out and takes the mirror fallback (itself a no-op off-Windows —
 *  there is no editor preview outside Win64, see issue #38 scope). */
class FDisplayXRPIEPreview
{
public:
	bool TryStart(TSharedRef<SViewport> /*ViewportWidget*/) { return false; }
	void Stop() {}
	bool IsActive() const { return false; }
};

#endif // PLATFORM_WINDOWS
