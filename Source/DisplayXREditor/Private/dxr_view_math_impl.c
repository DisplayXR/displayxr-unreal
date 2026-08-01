// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Compile shim: builds the shared displayxr::math type-neutral view
 *        math core into DisplayXREditor.
 *
 * As of displayxr-common v2.0.0 the OpenXR-typed wrappers (display3d_view.c,
 * camera3d_view.c) are pure pointer-casts over this TU, so it must be compiled
 * alongside them or every dxr_* symbol goes unresolved. DisplayXRCore does not
 * export these symbols, so the editor module compiles its own copy — the same
 * arrangement as the other two shims here.
 *
 * The canonical source lives in the displayxr-common submodule
 * (Source/ThirdParty/displayxr-common, pinned to a release tag). UBT only
 * compiles sources under module directories, so this shim pulls the .c in by
 * relative path — the submodule itself (including tests/selftest.c, which has
 * a main()) is never globbed.
 */

// The vendored TU shadows a few locals (C4456), which UE promotes to an
// error. Scope the suppression to this include so the module's own code
// keeps the check.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4456)
#endif

#include "../../ThirdParty/displayxr-common/include/dxr_view_math.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
