/* SPDX-License-Identifier: ISC
 * Switch 2 custom wallpaper loader (JPG via libturbojpeg, PNG via libpng).
 */
#ifndef HBMENU_SWITCH2_WALLPAPER_H
#define HBMENU_SWITCH2_WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>

bool switch2WallpaperLoad(const char *path);
void switch2WallpaperUnload(void);
bool switch2WallpaperIsLoaded(void);

/* Draws loaded wallpaper aspect-fill to 1280x720 with optional dimming (0..100%). */
void switch2WallpaperDraw(int dim_pct);

/* Draws loaded wallpaper into a preview box (x, y, w, h) with optional dimming. */
void switch2WallpaperDrawRect(float x, float y, float w, float h, int dim_pct);

#endif
