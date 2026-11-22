/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 */
#ifndef HBMENU_SWITCH2_GRAPHICS_VULKAN_H
#define HBMENU_SWITCH2_GRAPHICS_VULKAN_H

#include <stdint.h>
#include "render_scale.h"

struct HbmenuGraphicsConfig {
    /* The VkViSurfaceCreateInfoNN window: the default NWindow
     * (nwindowGetDefault), as on Switch 1. */
    void *window;
};

/* Set before upstream graphicsInit(). The window remains caller-owned
 * through graphicsExit(). */
int hbmenuGraphicsBind(const struct HbmenuGraphicsConfig *config);
int hbmenuGraphicsStatus(void);

#endif
