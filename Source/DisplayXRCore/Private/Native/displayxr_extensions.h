// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
//
// DisplayXR OpenXR extension definitions.
//
// This file no longer hand-declares the extension structs. It includes the
// runtime's canonical XR_EXT_*.h headers, vendored VERBATIM into ./openxr/
// from DisplayXR/displayxr-runtime at the commit pinned in
// `.displayxr-runtime-abi` (repo root). The `abi-guard` CI job diffs the
// vendored copies against that pinned ref and fails on any drift.
//
// WHY: hand-mirrored structs silently fall out of ABI sync when the runtime
// grows a field — the runtime parses next-chain structs by `type` only (no
// size check), so a short struct here makes it read trailing fields from
// uninitialized stack. That caused the black-window regression
// (XrWin32WindowBindingCreateInfoDXR grew transparentBackgroundEnabled /
// chromaKeyColor) and the rendering-mode stride bug (#234). Verbatim copies +
// a CI diff make any such drift a red check instead of a runtime mystery.
//
// DO NOT edit the vendored headers in ./openxr/ by hand. To adopt new runtime
// protocol: bump the SHA in `.displayxr-runtime-abi`, re-copy the headers from
// the runtime at that SHA, rebuild. The CI guard enforces the two stay in sync.

#pragma once

// We resolve every entry point through xrGetInstanceProcAddr and never link
// against an OpenXR loader, so suppress the prototype declarations the vendored
// headers would otherwise emit.
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif

#include <openxr/openxr.h>

// Cross-platform DisplayXR extensions.
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_atlas_capture.h>

// Window binding is platform-specific. Include exactly one — the Win32 and
// Cocoa headers both define PFN_xrReadbackCallback and XrCompositionLayer
// WindowSpaceEXT, so including both would be a duplicate-definition error.
#if PLATFORM_WINDOWS
#include <openxr/XR_DXR_win32_window_binding.h>
#elif PLATFORM_MAC
#include <openxr/XR_DXR_cocoa_window_binding.h>
#endif
