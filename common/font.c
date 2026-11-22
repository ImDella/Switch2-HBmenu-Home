#include "common.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef __SWITCH__
#define FONT_FACES_MAX PlSharedFontType_Total
#else
#define FONT_FACES_MAX 2
#endif

#ifdef __SWITCH__
static bool s_plinited;
#endif

static FT_Error s_font_libret=1, s_font_facesret[FONT_FACES_MAX];

static FT_Library s_font_library;
static FT_Face s_font_faces[FONT_FACES_MAX];
static FT_Face s_font_lastusedface;
static s32 s_font_faces_total = 0;

#ifdef __SWITCH2__
// The glyph cache (below) and the layout scale its glyphs were rendered at.
static void FontClearGlyphCache(void);
static u32 s_glyph_cache_num, s_glyph_cache_den;
#endif

static bool FontSetType(u32 font)
{
    s32 i=0;
    u32 scale=0;
    FT_Error ret=0;

    switch(font)
    {
        case interuiregular14:
            scale = 4;
        break;

        case interuiregular18:
            scale = 5;
        break;

        case interuimedium20:
            scale = 6;
        break;

        case fontscale7:
            scale = 7;
        break;

        case interuimedium30:
            scale = 8;
        break;

        case largestar:
            scale = 18;
        break;

        default:
            return false;
        break;
    }

    u32 char_height = scale*64;
    #ifdef __SWITCH2__
    // Scaled in 1/64th points, so a 1.5x size is not rounded to whole points.
    char_height = HBMENU_SCALE_COORD(char_height);
    // Glyphs are cached at one scale; a new one renders them again.
    if (s_glyph_cache_num != hbmenu_scale_num || s_glyph_cache_den != hbmenu_scale_den) {
        FontClearGlyphCache();
        s_glyph_cache_num = hbmenu_scale_num;
        s_glyph_cache_den = hbmenu_scale_den;
    }
    #endif

    for (i=0; i<s_font_faces_total; i++) {
        ret = FT_Set_Char_Size(
                s_font_faces[i],        /* handle to face object           */
                0,                      /* char_width in 1/64th of points  */
                char_height,            /* char_height in 1/64th of points */
                300,                    /* horizontal device resolution    */
                300);                   /* vertical device resolution      */

         if (ret) return false;
    }

    return true;
}

/*static inline const ffnt_page_t* FontGetPage(const ffnt_header_t* font, uint32_t page_id)
{
    //__builtin_printf("GetPage %u\n", (unsigned int)page_id);
    if (page_id >= font->npages)
        return NULL;
    ffnt_pageentry_t* ent = &((ffnt_pageentry_t*)(font+1))[page_id];
    if (ent->size == 0)
        return NULL;
    return (const ffnt_page_t*)((const uint8_t*)font + ent->offset);
}*/

#ifdef __SWITCH2__
static inline bool FontRenderGlyph(glyph_t* glyph, u32 font, uint32_t codepoint)
#else
static inline bool FontLoadGlyph(glyph_t* glyph, u32 font, uint32_t codepoint)
#endif
{
    FT_Face face=0;
    FT_Error ret=0;
    FT_GlyphSlot slot;
    FT_UInt glyph_index;
    FT_Bitmap* bitmap;
    s32 i=0;

    //__builtin_printf("LoadGlyph %u\n", (unsigned int)codepoint);
    /*const ffnt_page_t* page = FontGetPage(font, codepoint >> 8);
    if (!page)
        return false;

    codepoint &= 0xFF;
    uint32_t off = page->hdr.pos[codepoint];
    if (off == ~(uint32_t)0)
        return false;*/

    if (s_font_faces_total==0) return false;

    for (i=0; i<s_font_faces_total; i++) {
        face = s_font_faces[i];
        s_font_lastusedface = face;
        glyph_index = FT_Get_Char_Index(face, codepoint);
        if (glyph_index==0) continue;

        ret = FT_Load_Glyph(
                face,          /* handle to face object */
                glyph_index,   /* glyph index           */
                FT_LOAD_DEFAULT);

        if (ret==0)
        {
            ret = FT_Render_Glyph( face->glyph,             /* glyph slot  */
                                   FT_RENDER_MODE_NORMAL);  /* render mode */
        }

        if (ret) return false;

        break;
    }

    slot = face->glyph;
    bitmap = &slot->bitmap;

    //__builtin_printf("%c %u\n", (char)codepoint, (unsigned int)off);
    /*glyph->width   = page->hdr.widths[codepoint];
    glyph->height  = page->hdr.heights[codepoint];
    glyph->advance = page->hdr.advances[codepoint];
    glyph->posX    = page->hdr.posX[codepoint];
    glyph->posY    = page->hdr.posY[codepoint];
    glyph->data    = &page->data[off];*/

    glyph->width   = bitmap->width;
    glyph->height  = bitmap->rows;
    glyph->pitch   = bitmap->pitch;
    glyph->data    = bitmap->buffer;
    glyph->advance = slot->advance.x >> 6;
    glyph->posX    = slot->bitmap_left;
    glyph->posY    = slot->bitmap_top;
    return true;
}

#ifdef __SWITCH2__
/* Switch 2 keeps each glyph FreeType renders: its metrics, the face that
 * provided it (for the line height after a newline), and its position in
 * the GPU glyph atlas. Upstream renders every glyph again each frame. */
#define GLYPH_CACHE_SLOTS 4096

typedef struct {
    bool used;
    bool found;
    u32 font;
    uint32_t codepoint;
    uint32_t atlasGeneration;
    FT_Face face;
    glyph_t glyph;
} cachedGlyph_t;

static cachedGlyph_t s_glyph_cache[GLYPH_CACHE_SLOTS];
static u32 s_glyph_cache_used;

static void FontClearGlyphCache(void)
{
    memset(s_glyph_cache, 0, sizeof(s_glyph_cache));
    s_glyph_cache_used = 0;
}

static cachedGlyph_t* FontFindGlyphSlot(u32 font, uint32_t codepoint)
{
    // Knuth's multiplicative hash of the codepoint, offset by the font.
    u32 slot = (codepoint * 2654435761u + font) % GLYPH_CACHE_SLOTS;
    while (s_glyph_cache[slot].used) {
        if (s_glyph_cache[slot].font == font && s_glyph_cache[slot].codepoint == codepoint)
            return &s_glyph_cache[slot];
        slot = (slot + 1) % GLYPH_CACHE_SLOTS;
    }
    return &s_glyph_cache[slot];
}

static inline bool FontLoadGlyph(glyph_t* glyph, u32 font, uint32_t codepoint)
{
    if (s_font_faces_total==0) return false;

    cachedGlyph_t* entry = FontFindGlyphSlot(font, codepoint);
    // A visible glyph is drawn from the atlas; render it again if it is not
    // in the current atlas (the atlas filled up and was cleared).
    bool needsAtlas = entry->found && entry->glyph.width && entry->glyph.height;
    bool atlasCurrent = !needsAtlas || (entry->glyph.inAtlas &&
                        entry->atlasGeneration == renderGlyphAtlasGeneration());
    if (entry->used && atlasCurrent) {
        s_font_lastusedface = entry->face;
        *glyph = entry->glyph;
        return entry->found;
    }

    if (!entry->used && s_glyph_cache_used >= GLYPH_CACHE_SLOTS * 3 / 4) {
        FontClearGlyphCache();
        entry = FontFindGlyphSlot(font, codepoint);
    }

    glyph_t rendered = {0};
    bool found = FontRenderGlyph(&rendered, font, codepoint);
    if (found && rendered.width && rendered.height) {
        rendered.inAtlas = renderGlyphAtlasAdd(rendered.width, rendered.height,
                                               rendered.data, rendered.pitch,
                                               &rendered.atlasX, &rendered.atlasY);
    }
    rendered.data = NULL;

    if (!entry->used) s_glyph_cache_used++;
    entry->used = true;
    entry->found = found;
    entry->font = font;
    entry->codepoint = codepoint;
    entry->atlasGeneration = renderGlyphAtlasGeneration();
    entry->face = s_font_lastusedface;
    entry->glyph = rendered;
    *glyph = rendered;
    return found;
}
#endif

#ifdef __SWITCH2__
static void DrawGlyph(uint32_t x, uint32_t y, color_t clr, const glyph_t* glyph)
{
    // Upstream replaces the colour's alpha with the glyph coverage.
    if (!glyph->inAtlas) return;
    x += glyph->posX;
    y -= glyph->posY;
    renderGlyph((int32_t)x, (int32_t)y, glyph->width, glyph->height,
                glyph->atlasX, glyph->atlasY, clr.abgr | 0xff000000u);
}
#else
static void DrawGlyph(uint32_t x, uint32_t y, color_t clr, const glyph_t* glyph)
{
    uint32_t i, j;
    const uint8_t* data = glyph->data;
    x += glyph->posX;
    y -= glyph->posY; //y += glyph->posY;
    //__builtin_printf("DrawGlyph %u %u %08X\n", (unsigned int)x, (unsigned int)y, (unsigned int)clr.abgr);
    for (j = 0; j < glyph->height; j ++)
    {
        for (i = 0; i < glyph->width; i ++)
        {
            clr.a = data[i];
            if (!clr.a) continue;
            DrawPixel(x+i, y+j, clr);
        }
        data+= glyph->pitch;
    }
}
#endif

static inline uint8_t DecodeByte(const char** ptr)
{
    uint8_t c = (uint8_t)**ptr;
    *ptr += 1;
    return c;
}

// UTF-8 code adapted from http://www.json.org/JSON_checker/utf8_decode.c

static inline int8_t DecodeUTF8Cont(const char** ptr)
{
    int c = DecodeByte(ptr);
    return ((c & 0xC0) == 0x80) ? (c & 0x3F) : -1;
}

static inline uint32_t DecodeUTF8(const char** ptr)
{
    uint32_t r;
    uint8_t c;
    int8_t c1, c2, c3;

    c = DecodeByte(ptr);
    if ((c & 0x80) == 0)
        return c;
    if ((c & 0xE0) == 0xC0)
    {
        c1 = DecodeUTF8Cont(ptr);
        if (c1 >= 0)
        {
            r = ((c & 0x1F) << 6) | c1;
            if (r >= 0x80)
                return r;
        }
    } else if ((c & 0xF0) == 0xE0)
    {
        c1 = DecodeUTF8Cont(ptr);
        if (c1 >= 0)
        {
            c2 = DecodeUTF8Cont(ptr);
            if (c2 >= 0)
            {
                r = ((c & 0x0F) << 12) | (c1 << 6) | c2;
                if (r >= 0x800 && (r < 0xD800 || r >= 0xE000))
                    return r;
            }
        }
    } else if ((c & 0xF8) == 0xF0)
    {
        c1 = DecodeUTF8Cont(ptr);
        if (c1 >= 0)
        {
            c2 = DecodeUTF8Cont(ptr);
            if (c2 >= 0)
            {
                c3 = DecodeUTF8Cont(ptr);
                if (c3 >= 0)
                {
                    r = ((c & 0x07) << 18) | (c1 << 12) | (c2 << 6) | c3;
                    if (r >= 0x10000 && r < 0x110000)
                        return r;
                }
            }
        }
    }
    return 0xFFFD;
}

static void DrawText_(u32 font, uint32_t x, uint32_t y, color_t clr, const char* text, uint32_t max_width, const char* end_text)
{
    //__builtin_printf("DrawText %u %u %08X %s\n", (unsigned int)x, (unsigned int)y, (unsigned int)clr.abgr, text);
    //y += font->baseline;
    #ifdef __SWITCH2__
    x = HBMENU_SCALE_COORD(x);
    y = HBMENU_SCALE_COORD(y);
    max_width = HBMENU_SCALE_COORD(max_width);
    #endif
    uint32_t origX = x;
    if (s_font_faces_total==0) return;
    if (!FontSetType(font)) return;
    s_font_lastusedface = s_font_faces[0];

    while (*text)
    {
        if (max_width && x-origX >= max_width) {
            text = end_text;
            max_width = 0;
        }

        glyph_t glyph;
        uint32_t codepoint = DecodeUTF8(&text);

        if (codepoint == '\n')
        {
            if (max_width) {
                text = end_text;
                max_width = 0;
                continue;
            }

            x = origX;
            y += s_font_lastusedface->size->metrics.height / 64;
            continue;
        }

        if (!FontLoadGlyph(&glyph, font, codepoint))
        {
            if (!FontLoadGlyph(&glyph, font, '?'))
                continue;
        }

        DrawGlyph(x, y, clr, &glyph);
        x += glyph.advance;
    }
}

void DrawText(u32 font, uint32_t x, uint32_t y, color_t clr, const char* text)
{
    DrawText_(font, x, y, clr, text, 0, NULL);
}

void DrawTextFromLayout(ThemeLayoutId id, color_t clr, const char* text)
{
    ThemeLayoutObject *obj = &themeCurrent.layoutObjects[id];
    if (!obj->visible) return;

    DrawText(obj->font, obj->posStart[0], obj->posStart[1], clr, text);
}

void DrawTextFromLayoutRelative(ThemeLayoutId id, int base_x, int base_y, int *inPos, int *outPos, color_t clr, const char* text, const char align)
{
    ThemeLayoutObject *obj = &themeCurrent.layoutObjects[id];

    base_x = obj->posType ? base_x + inPos[0] : inPos[0];
    base_y = obj->posType ? base_y + inPos[1] : inPos[1];

    base_x = GetTextXCoordinate(obj->font, base_x, text, align);

    if (outPos) {
        outPos[0] = base_x;
        outPos[1] = base_y;
    }

    obj->posFinal[0] = base_x;
    obj->posFinal[1] = base_y;

    if (!obj->visible) return;

    GetTextDimensions(obj->font, text, &obj->textSize[0], &obj->textSize[1]);

    DrawText(obj->font, base_x, base_y, clr, text);
}

void DrawTextTruncate(u32 font, uint32_t x, uint32_t y, color_t clr, const char* text, uint32_t max_width, const char* end_text)
{
    DrawText_(font, x, y, clr, text, max_width, end_text);
}

void GetTextDimensions(u32 font, const char* text, uint32_t* width_out, uint32_t* height_out)
{
    uint32_t x = 0;
    uint32_t width = 0, height = 0;
    if (s_font_faces_total==0) return;
    if (!FontSetType(font)) return;
    s_font_lastusedface = s_font_faces[0];

    while (*text)
    {
        glyph_t glyph;
        uint32_t codepoint = DecodeUTF8(&text);

        if (codepoint == '\n')
        {
            x = 0;
            height += s_font_lastusedface->size->metrics.height / 64;
            continue;
        }

        if (!FontLoadGlyph(&glyph, font, codepoint))
        {
            if (!FontLoadGlyph(&glyph, font, '?'))
                continue;
        }

        x += glyph.advance;

        if (x > width)
            width = x;
    }

    #ifdef __SWITCH2__
    width = (width * hbmenu_scale_den + hbmenu_scale_num-1) / hbmenu_scale_num;
    height = (height * hbmenu_scale_den + hbmenu_scale_num-1) / hbmenu_scale_num;
    #endif
    if(width_out) *width_out = width;
    if(height_out) *height_out = height;
}

bool fontInitialize(void)
{
    FT_Error ret=0;
    s32 i;

    for (i=0; i<FONT_FACES_MAX; i++) s_font_facesret[i] = 1;

    ret = FT_Init_FreeType(&s_font_library);
    s_font_libret = ret;
    if (s_font_libret) return false;

    #ifdef __SWITCH__
    PlFontData fonts[PlSharedFontType_Total];

    Result rc=0;
    rc = plInitialize(PlServiceType_User);
    if (R_SUCCEEDED(rc)) {
        s_plinited = true;
        rc = plGetSharedFont(textGetLanguageCode(), fonts, FONT_FACES_MAX, &s_font_faces_total);
    }
    if (R_FAILED(rc)) return false;

    for (i=0; i<s_font_faces_total; i++) {
        ret = FT_New_Memory_Face( s_font_library,
                                  fonts[i].address,    /* first byte in memory */
                                  fonts[i].size,       /* size in bytes        */
                                  0,                   /* face_index           */
                                  &s_font_faces[i]);

        s_font_facesret[i] = ret;
        if (ret) return false;
    }
    #endif

    //These are loaded from "<original cwd>/fonts/<index>.ttf", these are user-supplied. Ideally these should be from plu SharedFont.
    #ifndef __SWITCH__
    char fontpath[PATH_MAX+1];

    for (i=0; i<FONT_FACES_MAX; i++) {
        memset(fontpath, 0, sizeof(fontpath));
        snprintf(fontpath, sizeof(fontpath)-1, "%s%sfonts%s%u.ttf", menuGetRootBasePath(), DIRECTORY_SEPARATOR, DIRECTORY_SEPARATOR, i);

        ret = FT_New_Face(        s_font_library,
                                  fontpath,
                                  0,
                                  &s_font_faces[i]);

        s_font_facesret[i] = ret;
        if (ret) return false;

        s_font_faces_total++;
    }
    #endif

    return true;
}

void fontExit()
{
    s32 i=0;

    for (i=0; i<s_font_faces_total; i++)
        if (s_font_facesret[i]==0) FT_Done_Face(s_font_faces[i]);

    if (s_font_libret==0) FT_Done_FreeType(s_font_library);

    #ifdef __SWITCH2__
    FontClearGlyphCache();
    #endif

    #ifdef __SWITCH__
    if (s_plinited) plExit();
    #endif
}

/*Automatically gives you the desired x-coordinate
 *based on the string length and desired alignment
 *rY=reference point... where to align around
 *align='t','b','c' translates to (top,bottom,center)
 *'t' aligned, top of text aligns with rY,
 *you get the rest....
 */
uint32_t GetTextYCoordinate(u32 font, uint32_t rY, const char* text, const char align) {
    uint32_t height_o,width;
    GetTextDimensions(font,text,&width,&height_o);
    uint32_t height = (uint32_t)height_o;
    uint32_t fC = (rY-height);

    switch(align){
        case 't':
        default:
            return rY;
        case 'c':
            return (rY+(height>>1));//>>1 is a bitwise shift for dividing by 2
        case 'b':
            if(fC<=0) return 0;
            else return fC;
    }
}

/*Automatically gives you the desired x-coordinate
 *based on the string length and desired alignment
 *rX=reference point... where to align around
 *text=string you want to display
 *align='r','l','c' translates to (right,left,center)
 *'r' aligned, rX location = end of string, you get the rest...
 */
uint32_t GetTextXCoordinate(u32 font, uint32_t rX, const char* text, const char align) {
    uint32_t height,width_o;
    GetTextDimensions(font,text,&width_o,&height);
    uint32_t fC = (rX-width_o);

    switch(align){
        case 'r':
            if(fC<0) return 0;
            else return fC;
        case 'c':
            return (rX-(width_o>>1));//>>1 is a bitwise shift for dividing by 2
        case 'l':
        default:
            return rX;
    }
}
