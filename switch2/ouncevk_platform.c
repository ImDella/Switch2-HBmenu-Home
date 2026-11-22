/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 * Upstream hbmenu as an ordinary libnx homebrew on libounce: libnx's default
 * __appInit runs, then userAppInit binds the default NWindow, whose VI2
 * layer graphics_vulkan.c presents to through OunceVK. OunceVK opens the GPU when
 * graphics_vulkan.c creates its Vulkan instance, and libounce's __appExit
 * closes it.
 */
#include <switch.h>

#include "graphics_vulkan.h"

void userAppInit(void);
void userAppInit(void)
{
    /* The console's default resolution in each mode: 1920x1080 handheld,
     * the TV output setting docked (3840x2160 at 4K). graphics_vulkan.c
     * recreates its swapchain when it changes. */
    nwindowSetModeDimensions(nwindowGetDefault(), 0, 0, 0, 0);
    const struct HbmenuGraphicsConfig config = {
        .window = nwindowGetDefault(),
    };
    hbmenuGraphicsBind(&config);
}
