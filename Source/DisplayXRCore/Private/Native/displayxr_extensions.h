// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
//
// Custom OpenXR extension constants and structs for DisplayXR support.
// These mirror the definitions in the CNSDK-OpenXR extension headers.

#pragma once

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- XR_EXT_display_info ---
#define XR_EXT_DISPLAY_INFO_EXTENSION_NAME "XR_EXT_display_info"
#define XR_EXT_DISPLAY_INFO_SPEC_VERSION 12

#define XR_TYPE_DISPLAY_INFO_EXT ((XrStructureType)1000999003)

typedef struct XrDisplayInfoEXT {
    XrStructureType type;
    void *next;
    XrExtent2Df displaySizeMeters;
    XrVector3f nominalViewerPositionInDisplaySpace;
    float recommendedViewScaleX;
    float recommendedViewScaleY;
    uint32_t displayPixelWidth;
    uint32_t displayPixelHeight;
} XrDisplayInfoEXT;

typedef enum XrDisplayModeEXT {
    XR_DISPLAY_MODE_2D_EXT = 0,
    XR_DISPLAY_MODE_3D_EXT = 1,
    XR_DISPLAY_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrDisplayModeEXT;

typedef XrResult(XRAPI_PTR *PFN_xrRequestDisplayModeEXT)(XrSession session, XrDisplayModeEXT displayMode);

// --- Display rendering mode (vendor-specific: SBS, anaglyph, lenticular, etc.) ---
typedef XrResult(XRAPI_PTR *PFN_xrRequestDisplayRenderingModeEXT)(XrSession session, uint32_t modeIndex);

#define XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT ((XrStructureType)1000999008)

typedef struct XrDisplayRenderingModeInfoEXT {
    XrStructureType type;
    void *next;
    uint32_t modeIndex;
    char modeName[XR_MAX_SYSTEM_NAME_SIZE];
    uint32_t viewCount;
    float viewScaleX;
    float viewScaleY;
    XrBool32 hardwareDisplay3D;
    uint32_t tileColumns;
    uint32_t tileRows;
    uint32_t viewWidthPixels;
    uint32_t viewHeightPixels;
    // v13 (DisplayXR runtime v1.4.0+): isActive marks the current mode for
    // this session; isRequestable is false for non-controller workspace
    // clients (runtime drops xrRequestDisplayRenderingModeEXT). Plugin
    // doesn't currently consume them, but the struct sizeof must match
    // the runtime's so xrEnumerateDisplayRenderingModesEXT writes modes[i]
    // at the right stride. See DisplayXR/displayxr-runtime#234.
    XrBool32 isActive;
    XrBool32 isRequestable;
} XrDisplayRenderingModeInfoEXT;

typedef XrResult(XRAPI_PTR *PFN_xrEnumerateDisplayRenderingModesEXT)(
    XrSession session,
    uint32_t modeCapacityInput,
    uint32_t *modeCountOutput,
    XrDisplayRenderingModeInfoEXT *modes);

// --- Shared texture output rect (canvas positioning for weaver alignment) ---
typedef XrResult (XRAPI_PTR *PFN_xrSetSharedTextureOutputRectEXT)(
    XrSession session, int32_t x, int32_t y, uint32_t width, uint32_t height);

// --- Readback callback (shared by macOS and Win32 bindings) ---
typedef void (*PFN_xrReadbackCallback)(const uint8_t *pixels, uint32_t width, uint32_t height, void *userdata);

// --- XR_EXT_win32_window_binding ---
#define XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_win32_window_binding"
#define XR_EXT_WIN32_WINDOW_BINDING_SPEC_VERSION 2

#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT ((XrStructureType)1000999002)

typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType type;
    const void *next;
    void *windowHandle; // HWND
    PFN_xrReadbackCallback readbackCallback;
    void *readbackUserdata;
    void *sharedTextureHandle; // D3D11 shared HANDLE
} XrWin32WindowBindingCreateInfoEXT;

typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType type;
    const void *next;
    XrCompositionLayerFlags layerFlags;
    XrSwapchainSubImage subImage;
    float x;         // Left edge, fraction of window [0..1]
    float y;         // Top edge, fraction of window [0..1]
    float width;     // Fraction of window [0..1]
    float height;    // Fraction of window [0..1]
    float disparity; // Horizontal shift, fraction of window
} XrCompositionLayerWindowSpaceEXT;

// --- XR_EXT_cocoa_window_binding ---
#define XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME "XR_EXT_cocoa_window_binding"
#define XR_EXT_COCOA_WINDOW_BINDING_SPEC_VERSION 3

#define XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT ((XrStructureType)1000999004)

typedef struct XrCocoaWindowBindingCreateInfoEXT {
    XrStructureType type;
    const void *next;
    void *viewHandle;                    // NSView* or NULL for offscreen
    PFN_xrReadbackCallback readbackCallback;
    void *readbackUserdata;
    void *sharedIOSurface;               // IOSurfaceRef for zero-copy GPU sharing
} XrCocoaWindowBindingCreateInfoEXT;

// --- XR_EXT_atlas_capture ---
// Runtime-owned "snapshot the multi-view atlas to a PNG". The app passes a
// pathPrefix (runtime appends "_atlas.png") and a stage; the runtime reads
// back its own compositor atlas — no app-side readback. Mirrors
// displayxr-runtime/src/external/openxr_includes/openxr/XR_EXT_atlas_capture.h.
#define XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME "XR_EXT_atlas_capture"
#define XR_EXT_ATLAS_CAPTURE_SPEC_VERSION 1

#define XR_TYPE_ATLAS_CAPTURE_INFO_EXT   ((XrStructureType)1000999120)
#define XR_TYPE_ATLAS_CAPTURE_RESULT_EXT ((XrStructureType)1000999121)

#define XR_ATLAS_CAPTURE_PATH_MAX_EXT 256

typedef enum XrAtlasCaptureStageEXT {
    XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT    = 0, // proj + window-space + quads
    XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT = 1, // projection-class layers only
    XR_ATLAS_CAPTURE_STAGE_MAX_ENUM_EXT        = 0x7FFFFFFF
} XrAtlasCaptureStageEXT;

typedef struct XrAtlasCaptureInfoEXT {
    XrStructureType type;
    const void *next;
    XrAtlasCaptureStageEXT stage;
    char pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_EXT];
} XrAtlasCaptureInfoEXT;

typedef struct XrAtlasCaptureResultEXT {
    XrStructureType type;
    void *next;
    uint64_t timestampNs;
    uint32_t atlasWidth;
    uint32_t atlasHeight;
    uint32_t eyeWidth;
    uint32_t eyeHeight;
    uint32_t tileColumns;
    uint32_t tileRows;
    float displayWidthM;
    float displayHeightM;
    float eyeLeftM[3];
    float eyeRightM[3];
} XrAtlasCaptureResultEXT;

typedef XrResult(XRAPI_PTR *PFN_xrCaptureAtlasEXT)(
    XrSession session,
    const XrAtlasCaptureInfoEXT *info,
    XrAtlasCaptureResultEXT *result);

#ifdef __cplusplus
}
#endif
