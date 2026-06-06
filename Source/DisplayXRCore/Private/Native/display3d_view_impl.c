// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Compile shim: builds the shared displayxr::math display3d implementation
 *        into DisplayXRCore.
 *
 * The canonical source lives in the displayxr-common submodule
 * (Source/ThirdParty/displayxr-common, pinned to a release tag). UBT only
 * compiles sources under module directories, so this shim pulls the .c in by
 * relative path — the submodule itself (including tests/selftest.c, which has
 * a main()) is never globbed.
 */

#include "../../../ThirdParty/displayxr-common/include/display3d_view.c"
