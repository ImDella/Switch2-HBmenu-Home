/* SPDX-License-Identifier: ISC
 * The upstream menu keeps its 1280x720 logical layout. On Switch 2 it is
 * drawn at the swapchain's size, which follows the console's default
 * resolution: 1920x1080 handheld, the TV output setting docked (3840x2160
 * at 4K). One logical pixel covers hbmenu_scale_num / hbmenu_scale_den
 * physical pixels, the swapchain height over 720: 3/2 at 1080p, 3/1 at
 * 2160p. graphics_vulkan.c sets it (renderSetScale) at the start of a frame.
 */
#ifndef HBMENU_SWITCH2_RENDER_SCALE_H
#define HBMENU_SWITCH2_RENDER_SCALE_H

#include <stdint.h>

#define HBMENU_LOGICAL_WIDTH 1280u
#define HBMENU_LOGICAL_HEIGHT 720u

extern uint32_t hbmenu_scale_num;
extern uint32_t hbmenu_scale_den;

#define HBMENU_SCALE_COORD(value) \
    (((value) * hbmenu_scale_num) / hbmenu_scale_den)

#endif
