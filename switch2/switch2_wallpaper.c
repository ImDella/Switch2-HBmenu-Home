/* SPDX-License-Identifier: ISC
 * Switch 2 custom wallpaper loader (JPG via libturbojpeg, PNG via libpng).
 * Automatically downscales oversized images to fit <= 1920x1080 RGBA32.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <turbojpeg.h>

#include "render.h"
#include "switch2_wallpaper.h"

#define WP_MAX_W 1920
#define WP_MAX_H 1080
#define WP_MAX_FILE_BYTES (24 * 1024 * 1024)

static uint8_t *s_wp_rgba = NULL;
static int s_wp_w = 0;
static int s_wp_h = 0;

static bool hasExtCI(const char *name, const char *ext) {
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    if (nl < el) return false;
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)name[nl - el + i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static uint8_t *downscaleRgbaIfNeeded(uint8_t *src, int *io_w, int *io_h) {
    int sw = *io_w, sh = *io_h;
    if (sw <= WP_MAX_W && sh <= WP_MAX_H)
        return src;

    float sx = (float)WP_MAX_W / (float)sw;
    float sy = (float)WP_MAX_H / (float)sh;
    float sc = sx < sy ? sx : sy;
    int dw = (int)((float)sw * sc);
    int dh = (int)((float)sh * sc);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    uint8_t *dst = malloc((size_t)dw * (size_t)dh * 4);
    if (!dst) {
        free(src);
        return NULL;
    }

    for (int y = 0; y < dh; y++) {
        int sy_i = (int)((int64_t)y * sh / dh);
        const uint8_t *srow = src + (size_t)sy_i * (size_t)sw * 4;
        uint8_t *drow = dst + (size_t)y * (size_t)dw * 4;
        for (int x = 0; x < dw; x++) {
            int sx_i = (int)((int64_t)x * sw / dw);
            memcpy(drow + (size_t)x * 4, srow + (size_t)sx_i * 4, 4);
        }
    }
    free(src);
    *io_w = dw;
    *io_h = dh;
    return dst;
}

static uint8_t *loadJpgFile(const char *path, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > WP_MAX_FILE_BYTES) {
        fclose(f);
        return NULL;
    }
    uint8_t *filebuf = malloc((size_t)sz);
    if (!filebuf) {
        fclose(f);
        return NULL;
    }
    if (fread(filebuf, 1, (size_t)sz, f) != (size_t)sz) {
        free(filebuf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    tjhandle tj = tjInitDecompress();
    if (!tj) {
        free(filebuf);
        return NULL;
    }

    int w = 0, h = 0, subsamp = 0, cs = 0;
    if (tjDecompressHeader3(tj, filebuf, (unsigned long)sz, &w, &h, &subsamp, &cs) != 0 ||
        w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        tjDestroy(tj);
        free(filebuf);
        return NULL;
    }

    /* Pick a scaling factor if image is huge so libturbojpeg decodes directly smaller */
    int num_sf = 0;
    tjscalingfactor *sfs = tjGetScalingFactors(&num_sf);
    int dw = w, dh = h;
    if (sfs && (w > WP_MAX_W || h > WP_MAX_H)) {
        for (int i = 0; i < num_sf; i++) {
            int sw = TJSCALED(w, sfs[i]);
            int sh = TJSCALED(h, sfs[i]);
            if (sw >= 1280 && sh >= 720 && sw <= WP_MAX_W && sh <= WP_MAX_H) {
                dw = sw;
                dh = sh;
                break;
            }
        }
    }

    uint8_t *rgba = malloc((size_t)dw * (size_t)dh * 4);
    if (!rgba) {
        tjDestroy(tj);
        free(filebuf);
        return NULL;
    }

    if (tjDecompress2(tj, filebuf, (unsigned long)sz, rgba, dw, dw * 4, dh, TJPF_RGBA, TJFLAG_FASTDCT) != 0) {
        free(rgba);
        tjDestroy(tj);
        free(filebuf);
        return NULL;
    }

    tjDestroy(tj);
    free(filebuf);

    rgba = downscaleRgbaIfNeeded(rgba, &dw, &dh);
    if (!rgba) return NULL;
    *out_w = dw;
    *out_h = dh;
    return rgba;
}

static uint8_t *loadPngFile(const char *path, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    png_byte sig[8];
    if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        fclose(f);
        return NULL;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(f);
        return NULL;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(f);
        return NULL;
    }

    uint8_t *rgba = NULL;
    png_bytep *rows = NULL;
    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        free(rgba);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return NULL;
    }

    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    int color_type = png_get_color_type(png, info);
    int bit_depth = png_get_bit_depth(png, info);

    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return NULL;
    }

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    rgba = malloc((size_t)w * (size_t)h * 4);
    rows = malloc((size_t)h * sizeof(png_bytep));
    if (!rgba || !rows) {
        free(rows);
        free(rgba);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return NULL;
    }
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = rgba + (size_t)y * (size_t)w * 4;

    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);

    int dw = (int)w, dh = (int)h;
    rgba = downscaleRgbaIfNeeded(rgba, &dw, &dh);
    if (!rgba) return NULL;
    *out_w = dw;
    *out_h = dh;
    return rgba;
}

void switch2WallpaperUnload(void) {
    if (s_wp_rgba) {
        renderForgetImage(s_wp_rgba);
        free(s_wp_rgba);
        s_wp_rgba = NULL;
    }
    s_wp_w = 0;
    s_wp_h = 0;
}

bool switch2WallpaperLoad(const char *path) {
    switch2WallpaperUnload();
    if (!path || !path[0])
        return false;

    int w = 0, h = 0;
    uint8_t *rgba = NULL;
    if (hasExtCI(path, ".png"))
        rgba = loadPngFile(path, &w, &h);
    else if (hasExtCI(path, ".jpg") || hasExtCI(path, ".jpeg"))
        rgba = loadJpgFile(path, &w, &h);

    if (!rgba)
        return false;

    s_wp_rgba = rgba;
    s_wp_w = w;
    s_wp_h = h;
    return true;
}

bool switch2WallpaperIsLoaded(void) {
    return s_wp_rgba != NULL && s_wp_w > 0 && s_wp_h > 0;
}

void switch2WallpaperDrawRect(float x, float y, float w, float h, int dim_pct) {
    if (!s_wp_rgba || s_wp_w <= 0 || s_wp_h <= 0)
        return;

    /* Aspect-fill (cover) inside [x, y, w, h] */
    float img_ar = (float)s_wp_w / (float)s_wp_h;
    float box_ar = w / h;
    float draw_w = w, draw_h = h;
    float draw_x = x, draw_y = y;

    if (img_ar > box_ar) {
        draw_h = h;
        draw_w = h * img_ar;
        draw_x = x - (draw_w - w) * 0.5f;
    } else {
        draw_w = w;
        draw_h = w / img_ar;
        draw_y = y - (draw_h - h) * 0.5f;
    }

    renderImageScaledF(s_wp_rgba, s_wp_w, s_wp_h, draw_x, draw_y, draw_w, draw_h,
                       RenderImageFormat_Rgba32, 0xffffffffu);

    if (dim_pct > 0) {
        if (dim_pct > 90) dim_pct = 90;
        uint8_t a = (uint8_t)((dim_pct * 255) / 100);
        uint32_t dim_col = ((uint32_t)a << 24); /* black with alpha */
        renderRectF(x, y, w, h, dim_col);
    }
}

void switch2WallpaperDraw(int dim_pct) {
    switch2WallpaperDrawRect(0.0f, 0.0f, 1280.0f, 720.0f, dim_pct);
}
