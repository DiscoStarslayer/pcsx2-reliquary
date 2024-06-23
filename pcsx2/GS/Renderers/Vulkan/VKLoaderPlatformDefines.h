// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+
#pragma once

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR

// vulkan.h pulls in windows.h on Windows, so we need to include our replacement header first
#include "common/RedtapeWindows.h"
#endif

#if defined(X11_API)
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#if defined(WAYLAND_API)
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#endif

