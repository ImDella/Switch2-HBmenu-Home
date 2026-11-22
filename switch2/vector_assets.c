/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 *
 * hbmenu's built-in icons as SVG on Switch 2. The build ships
 * switch2/vectors/<name>.svg in assets.zip in place of assets/<name>.bin.
 * Upstream's code keeps a bitmap of each asset at its layout size; this
 * rasterizes it from the SVG at load, and the renderer draws the vector
 * itself at the screen's resolution (render.h renderSetImageVector).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvgrast.h"

#include "vector_assets.h"

static void copyRaster(u8 *buffer, const u8 *rgba, size_t count, ImageMode mode) {
    if (mode == IMAGE_MODE_RGBA32) {
        memcpy(buffer, rgba, count * 4);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        buffer[i * 3 + 0] = rgba[i * 4 + 0];
        buffer[i * 3 + 1] = rgba[i * 4 + 1];
        buffer[i * 3 + 2] = rgba[i * 4 + 2];
    }
}

bool vectorAssetLoad(const char *bin_path, u8 *buffer, ImageMode mode,
                     size_t width, size_t height) {
    const char *extension = strrchr(bin_path, '.');
    if (!extension || strcasecmp(extension, ".bin") != 0 || !width || !height)
        return false;
    char svg_path[PATH_MAX];
    snprintf(svg_path, sizeof(svg_path), "%.*s.svg", (int)(extension - bin_path), bin_path);

    u8 *text = NULL;
    if (!assetsPhysfsReadFile(svg_path, &text, NULL, true))
        return false;
    NSVGimage *vector = nsvgParse((char *)text, "px", 96.0f);
    free(text);
    if (!vector)
        return false;
    if (vector->width <= 0.0f || vector->height <= 0.0f) {
        nsvgDelete(vector);
        return false;
    }

    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    u8 *rgba = malloc(width * height * 4);
    if (!rasterizer || !rgba) {
        free(rgba);
        if (rasterizer)
            nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(vector);
        return false;
    }
    nsvgRasterize(rasterizer, vector, 0, 0, width / vector->width, rgba,
                  (int)width, (int)height, (int)width * 4);
    nsvgDeleteRasterizer(rasterizer);
    copyRaster(buffer, rgba, width * height, mode);
    free(rgba);

    renderSetImageVector(buffer, vector);
    return true;
}
