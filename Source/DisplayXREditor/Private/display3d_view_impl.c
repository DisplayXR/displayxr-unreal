// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Compile shim: builds the shared displayxr::math display3d implementation
 *        into DisplayXREditor (the DisplayXRCore copy is not exported).
 *
 * Must stay a .c TU: the library implementation uses C compound literals,
 * which C++ rejects (C4576) — so it cannot be #included into a .cpp.
 * See Source/DisplayXRCore/Private/Native/display3d_view_impl.c for the
 * submodule/shim pattern.
 */

#include "../../ThirdParty/displayxr-common/include/display3d_view.c"
