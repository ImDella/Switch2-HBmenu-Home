/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 */
#pragma once

#include "../common/common.h"

/* Loads a built-in asset from the SVG beside its .bin path in assets.zip
 * (switch2/vectors): rasterizes it into `buffer` at the asset's layout size
 * and mode, the bitmap upstream's code uses, and gives the vector to the
 * renderer, which draws it at the screen's resolution. Returns false when
 * the asset has no SVG. */
bool vectorAssetLoad(const char *bin_path, u8 *buffer, ImageMode mode,
                     size_t width, size_t height);
