/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 *
 * Interface between the draw list (render.c) and the Vulkan backend
 * (graphics_vulkan.c).
 */
#ifndef HBMENU_SWITCH2_RENDER_BACKEND_H
#define HBMENU_SWITCH2_RENDER_BACKEND_H

#include <stdint.h>

/* A backend texture: an index into graphics_vulkan.c's texture table. */
typedef int32_t RenderTexture;
enum { RenderTexture_None = -1 };

/* One textured quad in physical pixels. Texture coordinates are
 * normalised; `color` (color_t.abgr) multiplies the texel. */
struct RenderQuad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    uint32_t color;
    RenderTexture texture;
};

enum RenderTextureFormat {
    RenderTextureFormat_Rgba8,
    RenderTextureFormat_Alpha8,  /* sampled as (1, 1, 1, value) */
};

/* Nearest for textures drawn one texel per physical pixel (glyphs, vectors,
 * the gradient table); linear for images scaled to the screen. */
enum RenderTextureFilter {
    RenderTextureFilter_Nearest,
    RenderTextureFilter_Linear,
};

/* --- Provided by the backend --------------------------------------------- */

/* Returns RenderTexture_None when the GPU is unavailable. */
RenderTexture renderBackendCreateTexture(uint32_t width, uint32_t height,
                                         enum RenderTextureFormat format,
                                         enum RenderTextureFilter filter);

/* Returns tightly packed memory for a region of the texture. Its contents
 * are copied to the texture before this frame's draws. NULL on failure. */
void *renderBackendUpload(RenderTexture texture, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height);

/* Destroys the texture once no submitted frame can still sample it. Safe to
 * call from any thread. */
void renderBackendReleaseTexture(RenderTexture texture);

/* --- Provided by the draw list ------------------------------------------- */

/* After the GPU is ready: creates the atlas and gradient textures. */
void renderInit(void);
/* Before the GPU is torn down: forgets every texture and frees CPU state. */
void renderShutdown(void);
/* At the start of a frame: clears the quad list. */
void renderFrameBegin(void);
/* At the end of a frame: closes open runs, queues atlas and gradient
 * uploads, and returns the frame's quads. */
const struct RenderQuad *renderFrameFinish(uint32_t *count);

#endif
