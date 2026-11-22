/* SPDX-License-Identifier: ISC
 * Switch 2 HOME Menu UI for ounce-hbmenu.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libconfig.h>
#include "switch2_ui.h"
#include "switch2_sfx.h"
#include "render.h"
#include "../common/netloader.h"

extern uint8_t *folder_icon_large;
extern uint8_t *invalid_icon_large;
extern uint8_t *theme_icon_large;

extern void themeMenuStartup(void);
extern void menuStartupCommon(void);
extern bool menuIsThemeMenuActive(void);

typedef enum {
    S2View_Carousel = 0,
    S2View_Grid = 1,
    S2View_Settings = 2,
} S2ViewMode;

typedef enum {
    S2Focus_Apps = 0,
    S2Focus_Pill = 1,
} S2FocusZone;

enum {
    PILL_NETLOADER = 0,
    PILL_FAVORITES = 1,
    PILL_ALL_SOFTWARE = 2,
    PILL_THEMES = 3,
    PILL_SETTINGS = 4,
    PILL_EXIT = 5,
    PILL_COUNT = 6,
};

enum {
    SET_SOUNDS = 0,
    SET_THEME = 1,
    SET_RING_SPEED = 2,
    SET_TRANSITIONS = 3,
    SET_DEFAULT_VIEW = 4,
    SET_SORT_ORDER = 5,
    SET_SHOW_SUBTITLE = 6,
    SET_COUNT = 7,
};

typedef struct {
    int ui_sounds;      /* 0 = Off, 1 = On */
    int theme_mode;     /* 0 = Auto, 1 = Light, 2 = Dark */
    int ring_speed;     /* 0 = Slow, 1 = Normal, 2 = Fast, 3 = Static */
    int transitions;    /* 0 = Slide & Fade, 1 = Fade, 2 = Instant */
    int default_view;   /* 0 = Carousel, 1 = All Software */
    int sort_order;     /* 0 = Favorites First, 1 = Alphabetical */
    int show_subtitle;  /* 0 = Off, 1 = On */
} Switch2Settings;

static Switch2Settings s_cfg = {
    .ui_sounds = 1,
    .theme_mode = 0,
    .ring_speed = 1,
    .transitions = 0,
    .default_view = 0,
    .sort_order = 0,
    .show_subtitle = 1,
};
static ThemePreset s_system_preset = THEME_PRESET_DARK;
static S2ViewMode s_prev_view = S2View_Carousel;
static S2ViewMode s_last_view = S2View_Carousel;
static float s_trans_t = 1.0f;
static int s_settings_sel = 0;

static float transSlideOffset(void);
static void triggerTransition(void);

/* Layout constants (logical 1280x720 coordinates, scaled natively to 1080p/4K) */
#define SCREEN_W 1280.0f
#define SCREEN_H 720.0f

#define CAROUSEL_START_X 96.0f
#define CAROUSEL_TILE_Y  196.0f
#define CAROUSEL_TILE_SZ 256.0f
#define CAROUSEL_STRIDE  272.0f

#define GRID_COLS        5
#define GRID_START_X     160.0f
#define GRID_START_Y     166.0f
#define GRID_TILE_SZ     154.0f
#define GRID_STRIDE_X    198.0f
#define GRID_STRIDE_Y    172.0f
#define GRID_CLIP_TOP    150.0f
#define GRID_CLIP_BOTTOM 506.0f

#define PILL_W           540.0f
#define PILL_H           86.0f
#define PILL_X           ((SCREEN_W - PILL_W) * 0.5f)
#define PILL_Y           516.0f
#define PILL_CENTER_Y    (PILL_Y + PILL_H * 0.5f)
#define PILL_BTN_STRIDE  82.0f
#define PILL_BTN_RADIUS  27.0f

#define MAX_VISIBLE_ENTRIES 1024

static S2ViewMode s_view = S2View_Carousel;
static S2FocusZone s_focus = S2Focus_Apps;
static int s_pill_idx = 0;
static bool s_favorites_only = false;
static bool s_exit_requested = false;

/* Visible filtered entry list (rebuilt cleanly per frame/input) */
static menuEntry_s *s_visible[MAX_VISIBLE_ENTRIES];
static int s_visible_indices[MAX_VISIBLE_ENTRIES]; /* index in menu_s */
static int s_visible_count = 0;
static int s_sel = 0; /* index in s_visible; == s_visible_count means "All Software" end tile in carousel */

/* Smooth 60fps animation state */
static float s_scroll_x = 0.0f;
static float s_grid_scroll_y = 0.0f;
static float s_app_pop = 1.0f;
static float s_app_pop_vel = 0.0f;
static float s_pill_pop = 1.0f;
static float s_pill_pop_vel = 0.0f;
static int s_last_sel = -1;
static int s_last_pill = -1;
static S2FocusZone s_last_focus = S2Focus_Apps;

/* Touch state */
static bool s_touch_active = false;
static bool s_touch_is_tap = false;
static int s_touch_start_x = 0, s_touch_start_y = 0;
static int s_touch_prev_x = 0, s_touch_prev_y = 0;
static float s_touch_vel_x = 0.0f;

static bool isDarkTheme(void) {
    /* Infer dark/light from current background luminance so custom .cfg themes work too */
    color_t bg = themeCurrent.backgroundColor;
    int lum = (bg.r * 299 + bg.g * 587 + bg.b * 114) / 1000;
    return lum < 128;
}

static uint32_t makeRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static color_t makeColorMuted(color_t base, uint8_t alpha) {
    return MakeColor(base.r, base.g, base.b, alpha);
}

/* DrawText() uses y as the baseline, and GetTextDimensions() returns height=0
 * for single-line text. This returns half the visual cap-height so text drawn
 * at (center_y + fontBaselineOffset(font)) is visually centered on center_y. */
static float fontBaselineOffset(u32 font) {
    switch (font) {
    case interuiregular14: return 5.5f;
    case interuiregular18: return 7.0f;
    case interuimedium20:  return 8.5f;
    case interuimedium30:  return 11.5f;
    default:               return 7.0f;
    }
}

static float ringTimer(void) {
    switch (s_cfg.ring_speed) {
    case 0:  return (float)menuTimer * 0.45f; /* Slow */
    case 1:  return (float)menuTimer * 1.0f;  /* Normal */
    case 2:  return (float)menuTimer * 2.0f;  /* Fast */
    case 3:  return 0.0f;                     /* Static */
    default: return (float)menuTimer;
    }
}

static void getSettingsPath(char *out, size_t out_size) {
    snprintf(out, out_size, "%s/config/nx-hbmenu/switch2_settings.cfg", menuGetRootBasePath());
}

static void switch2ApplyThemeSetting(void) {
    ThemePreset target = s_system_preset;
    if (s_cfg.theme_mode == 1) target = THEME_PRESET_LIGHT;
    else if (s_cfg.theme_mode == 2) target = THEME_PRESET_DARK;
    themeStartup(target);
    menuStartupCommon();
}

static void switch2SettingsSave(void) {
    config_t cfg;
    config_init(&cfg);
    config_setting_t *root = config_root_setting(&cfg);

    config_setting_t *s;
    s = config_setting_add(root, "ui_sounds", CONFIG_TYPE_INT);     if (s) config_setting_set_int(s, s_cfg.ui_sounds);
    s = config_setting_add(root, "theme_mode", CONFIG_TYPE_INT);    if (s) config_setting_set_int(s, s_cfg.theme_mode);
    s = config_setting_add(root, "ring_speed", CONFIG_TYPE_INT);    if (s) config_setting_set_int(s, s_cfg.ring_speed);
    s = config_setting_add(root, "transitions", CONFIG_TYPE_INT);   if (s) config_setting_set_int(s, s_cfg.transitions);
    s = config_setting_add(root, "default_view", CONFIG_TYPE_INT);  if (s) config_setting_set_int(s, s_cfg.default_view);
    s = config_setting_add(root, "sort_order", CONFIG_TYPE_INT);    if (s) config_setting_set_int(s, s_cfg.sort_order);
    s = config_setting_add(root, "show_subtitle", CONFIG_TYPE_INT); if (s) config_setting_set_int(s, s_cfg.show_subtitle);

    char path[PATH_MAX + 64];
    getSettingsPath(path, sizeof(path));
    config_write_file(&cfg, path);
    config_destroy(&cfg);
}

static void switch2SettingsLoad(void) {
    config_t cfg;
    config_init(&cfg);
    char path[PATH_MAX + 64];
    getSettingsPath(path, sizeof(path));
    if (config_read_file(&cfg, path) == CONFIG_TRUE) {
        int v = 0;
        if (config_lookup_int(&cfg, "ui_sounds", &v))     s_cfg.ui_sounds     = (v != 0) ? 1 : 0;
        if (config_lookup_int(&cfg, "theme_mode", &v))    s_cfg.theme_mode    = (v >= 0 && v <= 2) ? v : 0;
        if (config_lookup_int(&cfg, "ring_speed", &v))    s_cfg.ring_speed    = (v >= 0 && v <= 3) ? v : 1;
        if (config_lookup_int(&cfg, "transitions", &v))   s_cfg.transitions   = (v >= 0 && v <= 2) ? v : 0;
        if (config_lookup_int(&cfg, "default_view", &v))  s_cfg.default_view  = (v != 0) ? 1 : 0;
        if (config_lookup_int(&cfg, "sort_order", &v))    s_cfg.sort_order    = (v != 0) ? 1 : 0;
        if (config_lookup_int(&cfg, "show_subtitle", &v)) s_cfg.show_subtitle = (v != 0) ? 1 : 0;
    }
    config_destroy(&cfg);
}

void switch2UiInit(ThemePreset system_preset) {
    s_system_preset = system_preset;
    switch2SettingsLoad();
    switch2SfxSetEnabled(s_cfg.ui_sounds != 0);
    if (s_cfg.theme_mode != 0)
        switch2ApplyThemeSetting();
    s_view = (s_cfg.default_view == 1) ? S2View_Grid : S2View_Carousel;
    s_last_view = s_view;
    s_trans_t = 1.0f;
}

static bool hasEndAllSoftwareTile(void) {
    return s_view == S2View_Carousel && !menuIsThemeMenuActive() && s_visible_count > 0;
}

typedef struct {
    menuEntry_s *me;
    int orig_idx;
} VisiblePair;

static int cmpVisibleAlpha(const void *a, const void *b) {
    const VisiblePair *pa = (const VisiblePair *)a;
    const VisiblePair *pb = (const VisiblePair *)b;
    /* Keep folders grouped before files, then pure case-insensitive alphabetical */
    int fa = (pa->me->type == ENTRY_TYPE_FOLDER) ? 0 : 1;
    int fb = (pb->me->type == ENTRY_TYPE_FOLDER) ? 0 : 1;
    if (fa != fb) return fa - fb;
    return strcasecmp(pa->me->name, pb->me->name);
}

static void rebuildVisibleList(menu_s *menu) {
    s_visible_count = 0;
    if (!menu) return;

    bool filter_fav = s_favorites_only && !menuIsThemeMenuActive();
    VisiblePair pairs[MAX_VISIBLE_ENTRIES];
    int idx = 0;
    for (menuEntry_s *me = menu->firstEntry; me && s_visible_count < MAX_VISIBLE_ENTRIES; me = me->next, idx++) {
        if (filter_fav && !me->starred)
            continue;
        pairs[s_visible_count].me = me;
        pairs[s_visible_count].orig_idx = idx;
        s_visible_count++;
    }

    if (s_cfg.sort_order == 1 && !menuIsThemeMenuActive() && s_visible_count > 1) {
        qsort(pairs, (size_t)s_visible_count, sizeof(VisiblePair), cmpVisibleAlpha);
    }

    for (int i = 0; i < s_visible_count; i++) {
        s_visible[i] = pairs[i].me;
        s_visible_indices[i] = pairs[i].orig_idx;
    }

    int max_sel = hasEndAllSoftwareTile() ? s_visible_count : (s_visible_count > 0 ? s_visible_count - 1 : 0);
    if (s_sel > max_sel) s_sel = max_sel;
    if (s_sel < 0) s_sel = 0;

    if (s_visible_count > 0 && s_sel < s_visible_count) {
        menu->curEntry = s_visible_indices[s_sel];
    }
}

static void syncSelectionFromMenu(menu_s *menu) {
    rebuildVisibleList(menu);
    if (!menu || s_visible_count == 0) {
        s_sel = 0;
        return;
    }
    for (int i = 0; i < s_visible_count; i++) {
        if (s_visible_indices[i] == menu->curEntry) {
            s_sel = i;
            return;
        }
    }
    if (s_sel >= s_visible_count)
        s_sel = s_visible_count - 1;
    menu->curEntry = s_visible_indices[s_sel];
}

static float pillButtonCenterX(int index) {
    float start_x = SCREEN_W * 0.5f - (PILL_COUNT - 1) * 0.5f * PILL_BTN_STRIDE;
    return start_x + index * PILL_BTN_STRIDE;
}

static void triggerPillAction(int idx) {
    menu_s *menu = menuGetCurrent();
    switch (idx) {
    case PILL_NETLOADER:
        if (!menuIsThemeMenuActive()) {
            switch2SfxPlay(S2Sfx_Confirm);
            launchMenuNetloaderTask();
        }
        break;
    case PILL_FAVORITES:
        if (!menuIsThemeMenuActive()) {
            switch2SfxPlay(S2Sfx_Confirm);
            s_favorites_only = !s_favorites_only;
            s_sel = 0;
            s_scroll_x = 0.0f;
            s_grid_scroll_y = 0.0f;
            rebuildVisibleList(menu);
            if (s_visible_count > 0)
                s_focus = S2Focus_Apps;
            triggerTransition();
        }
        break;
    case PILL_ALL_SOFTWARE:
        if (!menuIsThemeMenuActive()) {
            switch2SfxPlay(S2Sfx_Confirm);
            s_view = (s_view == S2View_Carousel) ? S2View_Grid : S2View_Carousel;
            if (s_sel >= s_visible_count && s_visible_count > 0)
                s_sel = s_visible_count - 1;
            s_focus = S2Focus_Apps;
            triggerTransition();
        }
        break;
    case PILL_THEMES:
        switch2SfxPlay(S2Sfx_Confirm);
        if (menuIsThemeMenuActive()) {
            launchMenuBackTask();
            s_view = S2View_Carousel;
            s_focus = S2Focus_Apps;
            s_sel = 0;
            s_scroll_x = 0.0f;
        } else {
            s_view = S2View_Carousel;
            s_focus = S2Focus_Apps;
            themeMenuStartup();
            syncSelectionFromMenu(menuGetCurrent());
        }
        triggerTransition();
        break;
    case PILL_SETTINGS:
        switch2SfxPlay(S2Sfx_Confirm);
        s_prev_view = (s_view == S2View_Grid) ? S2View_Grid : S2View_Carousel;
        s_view = S2View_Settings;
        s_settings_sel = 0;
        break;
    case PILL_EXIT:
        switch2SfxPlay(S2Sfx_Back);
        s_exit_requested = true;
        break;
    }
}

/* --- Drawing helpers ------------------------------------------------------ */

static void drawIconAlpha(int x, int y, int width, int height, const uint8_t *image, color_t color) {
    renderImage(image, x, y, width, height, RenderImageFormat_AlphaMask, color.abgr | 0xff000000u);
}

static void drawHorizontalBattery(float x, float y, uint32_t charge, bool charging, color_t fg, bool dark) {
    /* Authentic Switch 2 horizontal battery icon (36x19 + 3px nub) */
    uint32_t col = fg.abgr | 0xff000000u;
    uint32_t bg = dark ? makeRGBA(24, 24, 26, 255) : makeRGBA(235, 235, 238, 255);
    renderRoundedRectF(x, y, 36.0f, 19.0f, 4.0f, col);
    renderRoundedRectF(x + 2.2f, y + 2.2f, 31.6f, 14.6f, 2.2f, bg);
    /* Right positive terminal nub */
    renderRoundedRectF(x + 36.5f, y + 5.5f, 3.0f, 8.0f, 1.5f, col);
    /* Charge fill bar */
    float fill_max = 27.6f;
    float fill_w = fill_max * ((float)charge / 100.0f);
    if (fill_w < 2.0f && charge > 0) fill_w = 2.0f;
    uint32_t fill_col = col;
    if (charging)
        fill_col = makeRGBA(82, 224, 96, 255);
    else if (charge <= 15)
        fill_col = makeRGBA(236, 56, 72, 255);
    if (fill_w > 0.0f)
        renderRoundedRectF(x + 4.2f, y + 4.2f, fill_w, 10.6f, 1.5f, fill_col);
}

static void drawTopBar(menu_s *menu) {
    bool dark = isDarkTheme();
    color_t txt = themeCurrent.textColor;
    const float bar_cy = 60.0f;

    /* Left: Switch 2 circular profile-style icon (with crisp white rim) */
    float cx = 74.0f, cy = bar_cy, br = 24.0f;
    uint32_t badge_bg = dark ? makeRGBA(46, 48, 56, 255) : makeRGBA(255, 255, 255, 255);
    uint32_t badge_rim = dark ? makeRGBA(190, 194, 204, 255) : makeRGBA(185, 190, 200, 255);
    renderSoftShadowF(cx - br, cy - br + 2.0f, br * 2.0f, br * 2.0f, br, 6.0f, dark ? 55 : 26);
    renderCircleF(cx, cy, br + 2.0f, badge_rim);
    renderCircleF(cx, cy, br, badge_bg);
    /* Mini Switch 2 Joy-Con dual icon inside circular badge */
    renderRoundedRectF(cx - 11.0f, cy - 10.5f, 9.5f, 21.0f, 4.75f, makeRGBA(0, 195, 227, 255));
    renderRoundedRectF(cx + 1.5f, cy - 10.5f, 9.5f, 21.0f, 4.75f, makeRGBA(255, 69, 84, 255));

    const char *title = "Homebrew Menu";
    uint32_t tw = 0, th = 0;
    GetTextDimensions(interuiregular18, title, &tw, &th);
    DrawText(interuiregular18, 114, (uint32_t)(bar_cy + fontBaselineOffset(interuiregular18)), makeColorMuted(txt, dark ? 210 : 200), title);

    float tag_x = 114.0f + (float)tw + 14.0f;

    /* Context pill tag (Theme Menu / Favorites / All Software / Subfolder) */
    const char *tag_text = NULL;
    uint32_t tag_bg = dark ? makeRGBA(52, 54, 62, 255) : makeRGBA(218, 221, 228, 255);
    color_t tag_fg = txt;
    if (menuIsThemeMenuActive()) {
        tag_text = "Themes";
        tag_bg = makeRGBA(124, 92, 255, 255);
        tag_fg = MakeColor(255, 255, 255, 255);
    } else if (s_favorites_only) {
        tag_text = "Favorites";
        tag_bg = makeRGBA(240, 162, 24, 255);
        tag_fg = MakeColor(255, 255, 255, 255);
    } else if (s_view == S2View_Grid) {
        tag_text = "All Software";
        tag_bg = makeRGBA(230, 40, 58, 255);
        tag_fg = MakeColor(255, 255, 255, 255);
    } else if (menu && strcmp(menu->dirname, menuGetRootPath()) != 0) {
        const char *slash = strrchr(menu->dirname, '/');
        tag_text = (slash && slash[1]) ? (slash + 1) : menu->dirname;
    }

    if (tag_text && tag_text[0]) {
        uint32_t pw = 0, ph = 0;
        GetTextDimensions(interuiregular14, tag_text, &pw, &ph);
        float pill_w = (float)pw + 22.0f;
        if (pill_w > 220.0f) pill_w = 220.0f;
        renderRoundedRectF(tag_x, bar_cy - 13.0f, pill_w, 26.0f, 13.0f, tag_bg);
        DrawTextTruncate(interuiregular14, (uint32_t)(tag_x + 11.0f), (uint32_t)(bar_cy + fontBaselineOffset(interuiregular14)),
                         tag_fg, tag_text, (uint32_t)(pill_w - 22.0f), "...");
    }

    /* Right side (all cleanly vertically centered on bar_cy, matching Switch 2 HOME):
     * [Applet Mode]   HH:MM   [Wi-Fi]   [Horizontal Battery] */
    float right_x = 1232.0f;

    /* Horizontal Battery */
    uint32_t batteryCharge = 100;
    bool isCharging = false;
    if (powerGetDetails(&batteryCharge, &isCharging)) {
        if (batteryCharge > 100) batteryCharge = 100;
        right_x -= 40.0f;
        drawHorizontalBattery(right_x, bar_cy - 9.5f, batteryCharge, isCharging, txt, dark);
        right_x -= 22.0f;
    }

    /* Network icon */
    bool netFlag = false, tempFlag = false;
    s32 temperature = 0;
    AssetId netId = AssetId_wifi_none_icon;
    if (statusGet(&netFlag, &netId, &tempFlag, &temperature) && netFlag) {
        assetsDataEntry *netData = NULL;
        assetsGetData(netId, &netData);
        if (netData) {
            int nw = netData->imageSize[0];
            int nh = netData->imageSize[1];
            right_x -= (float)nw;
            drawIconAlpha((int)right_x, (int)(bar_cy - (float)nh * 0.5f), nw, nh, netData->buffer, txt);
            right_x -= 20.0f;
        }
    }

    /* Clock (HH:MM) */
    time_t unixTime = time(NULL);
    struct tm *tmNow = localtime(&unixTime);
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", tmNow->tm_hour, tmNow->tm_min);
    uint32_t cw = 0, ch = 0;
    GetTextDimensions(interuimedium20, timeStr, &cw, &ch);
    right_x -= (float)cw;
    DrawText(interuimedium20, (uint32_t)right_x, (uint32_t)(bar_cy + fontBaselineOffset(interuimedium20)), txt, timeStr);
    right_x -= 18.0f;

    /* Applet Mode pill badge (only when running in Applet Mode) */
    #ifdef __SWITCH__
    AppletType at = appletGetAppletType();
    if (at != AppletType_Application && at != AppletType_SystemApplication) {
        const char *appletStr = "Applet Mode";
        uint32_t aw = 0, ah = 0;
        GetTextDimensions(interuiregular14, appletStr, &aw, &ah);
        float pw = (float)aw + 20.0f;
        right_x -= pw;
        renderRoundedRectF(right_x, bar_cy - 13.0f, pw, 26.0f, 13.0f, makeRGBA(225, 48, 66, 255));
        DrawText(interuiregular14, (uint32_t)(right_x + 10.0f), (uint32_t)(bar_cy + fontBaselineOffset(interuiregular14)),
                 MakeColor(255, 255, 255, 255), appletStr);
    }
    #endif
}

static void drawAppTile(menuEntry_s *me, float x, float y, float size, bool is_focused, float pop_amount) {
    bool dark = isDarkTheme();
    /* Subtle Switch 2 elastic pop (stays tight so neighboring tiles never collide) */
    float scale = is_focused ? (1.0f + 0.026f * pop_amount) : 1.0f;
    float draw_sz = size * scale;
    float dx = x - (draw_sz - size) * 0.5f;
    float dy = y - (draw_sz - size) * 0.5f;
    float radius = draw_sz * 0.072f;

    /* Soft Switch 2 elevation shadow */
    renderSoftShadowF(dx, dy + (is_focused ? 7.0f : 3.0f), draw_sz, draw_sz,
                      radius, is_focused ? 14.0f : 7.0f,
                      dark ? (is_focused ? 120 : 55) : (is_focused ? 65 : 30));

    /* Tile backing card (prevents transparent edges on SVG/PNG icons) */
    uint32_t card_bg = dark ? makeRGBA(38, 40, 46, 255) : makeRGBA(255, 255, 255, 255);
    renderRoundedRectF(dx, dy, draw_sz, draw_sz, radius, card_bg);

    /* Resolve icon image */
    const uint8_t *img = NULL;
    enum RenderImageFormat fmt = RenderImageFormat_Rgb24Rounded;
    int img_w = 256, img_h = 256;
    const ThemeLayoutObject *largeobj = &themeCurrent.layoutObjects[ThemeLayoutId_MenuActiveEntryIcon];
    if (largeobj->size[0] > 0 && largeobj->size[1] > 0) {
        img_w = largeobj->size[0];
        img_h = largeobj->size[1];
    }

    if (me->icon_gfx) {
        img = me->icon_gfx;
        fmt = (me->icon_mode == IMAGE_MODE_RGB24) ? RenderImageFormat_Rgb24Rounded : RenderImageFormat_Rgba32Rounded;
    } else if (me->type == ENTRY_TYPE_FOLDER) {
        img = folder_icon_large;
        fmt = RenderImageFormat_Rgba32Rounded;
    } else if (me->type == ENTRY_TYPE_THEME) {
        img = theme_icon_large;
        fmt = RenderImageFormat_Rgba32Rounded;
    } else {
        img = invalid_icon_large;
        fmt = RenderImageFormat_Rgba32Rounded;
    }

    if (img) {
        renderImageScaledF(img, img_w, img_h, dx, dy, draw_sz, draw_sz, fmt, 0xffffffffu);
    }

    /* Clean golden star in top-right corner for favorite apps (no backing circle) */
    if (me->starred && me->type != ENTRY_TYPE_THEME) {
        float bx = dx + draw_sz - 22.0f;
        float by = dy + 22.0f;
        renderStarF(bx, by + 1.5f, 11.5f, makeRGBA(0, 0, 0, 125));
        renderStarF(bx, by, 10.5f, makeRGBA(255, 206, 56, 255));
    }

    /* Switch 2 iridescent focus ring with crisp 2.5px inner gap */
    if (is_focused) {
        float pad = 6.5f;
        renderSwitch2FocusRingF(dx - pad, dy - pad, draw_sz + pad * 2.0f, draw_sz + pad * 2.0f,
                                radius + pad, 4.0f, ringTimer(),
                                themeCurrent.highlightColor.abgr,
                                themeCurrent.highlightGradientEdgeColor.abgr);
    }
}

static void drawAllSoftwareEndTile(float x, float y, float size, bool is_focused, float pop_amount) {
    bool dark = isDarkTheme();
    float scale = is_focused ? (1.0f + 0.068f * pop_amount) : 1.0f;
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float r = (size * 0.38f) * scale;

    uint32_t bg = dark ? makeRGBA(46, 48, 56, 255) : makeRGBA(255, 255, 255, 255);
    uint32_t fg = dark ? makeRGBA(225, 228, 236, 255) : makeRGBA(62, 66, 74, 255);

    renderSoftShadowF(cx - r, cy - r + 5.0f, r * 2.0f, r * 2.0f, r, is_focused ? 14.0f : 8.0f,
                      dark ? 95 : 45);
    renderCircleF(cx, cy, r, bg);

    /* 2x2 rounded squares grid icon */
    float sq = r * 0.34f;
    float gap = r * 0.12f;
    float off = sq + gap * 0.5f;
    renderRoundedRectF(cx - off, cy - off, sq, sq, sq * 0.25f, fg);
    renderRoundedRectF(cx + gap * 0.5f, cy - off, sq, sq, sq * 0.25f, fg);
    renderRoundedRectF(cx - off, cy + gap * 0.5f, sq, sq, sq * 0.25f, fg);
    renderRoundedRectF(cx + gap * 0.5f, cy + gap * 0.5f, sq, sq, sq * 0.25f, fg);

    if (is_focused) {
        renderSwitch2CircleFocusRingF(cx, cy, r + 6.0f, 5.0f, ringTimer(),
                                      themeCurrent.highlightColor.abgr,
                                      themeCurrent.highlightGradientEdgeColor.abgr);
    }
}

static void drawFocusedHeaderInfo(menuEntry_s *me, float center_x, float title_y, bool is_all_software_tile) {
    bool dark = isDarkTheme();
    /* Switch 2 HOME highlights the selected title in crisp cyan/teal right above the tile */
    color_t title_col = dark ? MakeColor(78, 224, 245, 255) : MakeColor(0, 168, 204, 255);
    color_t muted = makeColorMuted(themeCurrent.textColor, dark ? 150 : 145);

    if (is_all_software_tile) {
        const char *label = "All Software";
        uint32_t tw = 0, th = 0;
        GetTextDimensions(interuimedium20, label, &tw, &th);
        float tx = center_x - (float)tw * 0.5f;
        if (tx < 48.0f) tx = 48.0f;
        if (tx + (float)tw > SCREEN_W - 48.0f) tx = SCREEN_W - 48.0f - (float)tw;
        DrawText(interuimedium20, (uint32_t)tx, (uint32_t)title_y, title_col, label);
        return;
    }

    if (!me) return;

    char titleBuf[ENTRY_NAMELENGTH + 16];
    snprintf(titleBuf, sizeof(titleBuf), "%s", me->name);

    uint32_t max_w = 620;
    uint32_t tw = 0, th = 0;
    GetTextDimensions(interuimedium20, titleBuf, &tw, &th);
    if (tw > max_w) tw = max_w;

    /* Tiny Switch 2 cyan cartridge/NRO badge (or gold star if favorite) beside the title */
    bool show_star = (me->starred && me->type != ENTRY_TYPE_THEME);
    bool show_cart = (me->type == ENTRY_TYPE_FILE);
    float icon_w = (show_star ? 24.0f : 0.0f) + (show_cart ? 22.0f : 0.0f);
    float total_w = icon_w + (float)tw;
    float tx = center_x - total_w * 0.5f;
    if (tx < 48.0f) tx = 48.0f;
    if (tx + total_w > SCREEN_W - 48.0f) tx = SCREEN_W - 48.0f - total_w;

    float cur_x = tx;
    if (show_star) {
        renderStarF(cur_x + 9.0f, title_y - 7.5f, 9.0f, makeRGBA(255, 206, 56, 255));
        cur_x += 24.0f;
    }
    if (show_cart) {
        uint32_t c_col = title_col.abgr | 0xff000000u;
        renderRoundedRectF(cur_x, title_y - 16.0f, 14.0f, 18.0f, 3.0f, c_col);
        renderRectF(cur_x + 3.5f, title_y - 4.0f, 7.0f, 2.0f, dark ? makeRGBA(24, 24, 26, 255) : makeRGBA(235, 235, 238, 255));
        cur_x += 22.0f;
    }

    DrawTextTruncate(interuimedium20, (uint32_t)cur_x, (uint32_t)title_y, title_col, titleBuf, max_w, "...");

    /* Discreet Author · Version subtitle right underneath (respects Settings toggle) */
    if (!s_cfg.show_subtitle)
        return;
    if (me->type != ENTRY_TYPE_FOLDER) {
        char subBuf[320];
        if (me->version[0] && me->author[0])
            snprintf(subBuf, sizeof(subBuf), "%s  \u00B7  v%s", me->author, me->version);
        else if (me->author[0])
            snprintf(subBuf, sizeof(subBuf), "%s", me->author);
        else if (me->version[0])
            snprintf(subBuf, sizeof(subBuf), "v%s", me->version);
        else
            subBuf[0] = 0;

        if (subBuf[0]) {
            uint32_t sw = 0, sh = 0;
            GetTextDimensions(interuiregular14, subBuf, &sw, &sh);
            if (sw > max_w) sw = max_w;
            float sx = center_x - (float)sw * 0.5f;
            if (sx < 48.0f) sx = 48.0f;
            if (sx + (float)sw > SCREEN_W - 48.0f) sx = SCREEN_W - 48.0f - (float)sw;
            DrawTextTruncate(interuiregular14, (uint32_t)sx, (uint32_t)(title_y + 24.0f), muted, subBuf, max_w, "...");
        }
    } else {
        const char *sub = "Folder";
        uint32_t sw = 0, sh = 0;
        GetTextDimensions(interuiregular14, sub, &sw, &sh);
        float sx = center_x - (float)sw * 0.5f;
        if (sx < 48.0f) sx = 48.0f;
        if (sx + (float)sw > SCREEN_W - 48.0f) sx = SCREEN_W - 48.0f - (float)sw;
        DrawText(interuiregular14, (uint32_t)sx, (uint32_t)(title_y + 24.0f), muted, sub);
    }
}

static void drawCarouselView(void) {
    int total_slots = s_visible_count + (hasEndAllSoftwareTile() ? 1 : 0);

    if (s_visible_count == 0) {
        const char *msg = s_favorites_only
            ? "No favorite apps yet. Press X on an app to add it to Favorites."
            : textGetString(StrId_NoAppsFound_Msg);
        uint32_t mw = 0, mh = 0;
        GetTextDimensions(interuiregular18, msg, &mw, &mh);
        DrawText(interuiregular18, (uint32_t)((SCREEN_W - (float)mw) * 0.5f), 315,
                 themeCurrent.textColor, msg);
        return;
    }

    /* Smooth horizontal spring camera */
    float content_w = CAROUSEL_START_X * 2.0f + (float)total_slots * CAROUSEL_STRIDE - (CAROUSEL_STRIDE - CAROUSEL_TILE_SZ);
    float max_scroll = content_w - SCREEN_W;
    if (max_scroll < 0.0f) max_scroll = 0.0f;

    if (!s_touch_active) {
        if (fabsf(s_touch_vel_x) > 0.4f) {
            s_scroll_x -= s_touch_vel_x;
            s_touch_vel_x *= 0.88f;
            if (s_scroll_x < 0.0f) { s_scroll_x = 0.0f; s_touch_vel_x = 0.0f; }
            if (s_scroll_x > max_scroll) { s_scroll_x = max_scroll; s_touch_vel_x = 0.0f; }
        } else {
            s_touch_vel_x = 0.0f;
            float active_center = CAROUSEL_START_X + (float)s_sel * CAROUSEL_STRIDE + CAROUSEL_TILE_SZ * 0.5f;
            float target_scroll = active_center - SCREEN_W * 0.42f;
            if (target_scroll < 0.0f) target_scroll = 0.0f;
            if (target_scroll > max_scroll) target_scroll = max_scroll;
            s_scroll_x += (target_scroll - s_scroll_x) * 0.22f;
            if (fabsf(target_scroll - s_scroll_x) < 0.25f) s_scroll_x = target_scroll;
        }
    }

    /* Draw unselected tiles first so the focused tile + shadow/ring always overlap cleanly on top */
    float slide_y = transSlideOffset();
    float tile_y = CAROUSEL_TILE_Y + slide_y;
    float focused_x = 0.0f;
    for (int i = 0; i < total_slots; i++) {
        float tx = CAROUSEL_START_X + (float)i * CAROUSEL_STRIDE - s_scroll_x;
        if (tx + CAROUSEL_TILE_SZ < -40.0f || tx > SCREEN_W + 40.0f)
            continue;
        bool is_active = (s_focus == S2Focus_Apps && i == s_sel);
        if (is_active) {
            focused_x = tx;
            continue;
        }
        if (i < s_visible_count)
            drawAppTile(s_visible[i], tx, tile_y, CAROUSEL_TILE_SZ, false, 0.0f);
        else
            drawAllSoftwareEndTile(tx, tile_y, CAROUSEL_TILE_SZ, false, 0.0f);
    }

    /* Draw focused tile on top + its Switch 2 title above */
    if (s_focus == S2Focus_Apps && s_sel >= 0 && s_sel < total_slots) {
        focused_x = CAROUSEL_START_X + (float)s_sel * CAROUSEL_STRIDE - s_scroll_x;
        bool is_end_tile = (s_sel == s_visible_count);
        if (!is_end_tile) {
            drawAppTile(s_visible[s_sel], focused_x, tile_y, CAROUSEL_TILE_SZ, true, s_app_pop);
            drawFocusedHeaderInfo(s_visible[s_sel], focused_x + CAROUSEL_TILE_SZ * 0.5f, 152.0f + slide_y, false);
        } else {
            drawAllSoftwareEndTile(focused_x, tile_y, CAROUSEL_TILE_SZ, true, s_app_pop);
            drawFocusedHeaderInfo(NULL, focused_x + CAROUSEL_TILE_SZ * 0.5f, 162.0f + slide_y, true);
        }
    }
}

static void drawGridView(void) {
    if (s_visible_count == 0) {
        drawCarouselView();
        return;
    }

    if (s_sel >= s_visible_count) s_sel = s_visible_count - 1;
    int sel_row = s_sel / GRID_COLS;
    int total_rows = (s_visible_count + GRID_COLS - 1) / GRID_COLS;
    float max_scroll_y = (total_rows > 2) ? (float)(total_rows - 2) * GRID_STRIDE_Y : 0.0f;
    float target_y = (sel_row > 0) ? (float)(sel_row - 1) * GRID_STRIDE_Y : 0.0f;
    if (target_y > max_scroll_y) target_y = max_scroll_y;
    s_grid_scroll_y += (target_y - s_grid_scroll_y) * 0.22f;

    /* Draw unselected grid tiles */
    float slide_y = transSlideOffset();
    for (int i = 0; i < s_visible_count; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        float tx = GRID_START_X + (float)col * GRID_STRIDE_X;
        float ty = GRID_START_Y + (float)row * GRID_STRIDE_Y - s_grid_scroll_y + slide_y;
        if (ty + GRID_TILE_SZ < GRID_CLIP_TOP - 20.0f || ty > GRID_CLIP_BOTTOM + 20.0f)
            continue;
        if (s_focus == S2Focus_Apps && i == s_sel)
            continue;
        drawAppTile(s_visible[i], tx, ty, GRID_TILE_SZ, false, 0.0f);
    }

    /* Draw focused grid tile on top */
    if (s_focus == S2Focus_Apps && s_sel >= 0 && s_sel < s_visible_count) {
        int col = s_sel % GRID_COLS;
        int row = s_sel / GRID_COLS;
        float tx = GRID_START_X + (float)col * GRID_STRIDE_X;
        float ty = GRID_START_Y + (float)row * GRID_STRIDE_Y - s_grid_scroll_y + slide_y;
        if (ty + GRID_TILE_SZ >= GRID_CLIP_TOP - 20.0f && ty <= GRID_CLIP_BOTTOM + 20.0f)
            drawAppTile(s_visible[s_sel], tx, ty, GRID_TILE_SZ, true, s_app_pop);
    }

    /* Clean top & bottom occlusion bands so scrolled grid tiles never overlap the header or pill */
    uint32_t bg_solid = themeCurrent.backgroundColor.abgr | 0xff000000u;
    renderRectF(0.0f, 0.0f, SCREEN_W, GRID_CLIP_TOP, bg_solid);
    renderRectF(0.0f, GRID_CLIP_BOTTOM, SCREEN_W, SCREEN_H - GRID_CLIP_BOTTOM, bg_solid);

    /* Centered title/author bar above the grid (drawn over the clean header band) */
    if (s_focus == S2Focus_Apps && s_sel < s_visible_count) {
        drawFocusedHeaderInfo(s_visible[s_sel], SCREEN_W * 0.5f, 112.0f, false);
    }
}

static void drawPillIconSymbol(int idx, float cx, float cy, float scale, uint32_t fg, uint32_t hole_bg) {
    float s = scale * 1.15f;
    switch (idx) {
    case PILL_NETLOADER:
        /* Crisp SDF Switch 2 style Download / Netloader icon (arrow + U tray) */
        renderNetloaderIconF(cx, cy, 17.5f * scale, fg);
        break;
    case PILL_FAVORITES:
        /* Crisp SDF 5-point star centered mathematically on (cx, cy) */
        renderStarF(cx, cy, 14.5f * scale, fg);
        break;
    case PILL_ALL_SOFTWARE: {
        /* 2x2 crisp rounded squares (Switch 2 style) */
        float sq = 9.0f * s;
        float gap = 3.6f * s;
        renderRoundedRectF(cx - sq - gap * 0.5f, cy - sq - gap * 0.5f, sq, sq, 2.4f * s, fg);
        renderRoundedRectF(cx + gap * 0.5f, cy - sq - gap * 0.5f, sq, sq, 2.4f * s, fg);
        renderRoundedRectF(cx - sq - gap * 0.5f, cy + gap * 0.5f, sq, sq, 2.4f * s, fg);
        renderRoundedRectF(cx + gap * 0.5f, cy + gap * 0.5f, sq, sq, 2.4f * s, fg);
        break;
    }
    case PILL_THEMES:
        /* Layered theme cards */
        renderRoundedRectF(cx - 11.5f * s, cy - 9.0f * s, 16.0f * s, 12.5f * s, 3.2f * s, fg);
        renderRoundedRectF(cx - 4.0f * s, cy - 2.0f * s, 16.0f * s, 12.5f * s, 3.2f * s,
                           (fg & 0x00ffffffu) | 0xc8000000u);
        break;
    case PILL_SETTINGS:
        /* Crisp Switch 2 8-tooth Settings Gear */
        renderSettingsIconF(cx, cy, 16.5f * scale, fg);
        break;
    case PILL_EXIT:
        /* Clean thin Switch 2 power icon */
        renderCircleF(cx, cy + 1.5f * s, 11.5f * s, fg);
        renderCircleF(cx, cy + 1.5f * s, 8.2f * s, hole_bg);
        renderRectF(cx - 4.5f * s, cy - 11.5f * s, 9.0f * s, 10.0f * s, hole_bg);
        renderRoundedRectF(cx - 1.8f * s, cy - 12.5f * s, 3.6f * s, 12.0f * s, 1.8f * s, fg);
        break;
    }
}

static void drawBottomPillBar(void) {
    bool dark = isDarkTheme();

    /* Authentic Switch 2 floating pill dock: clean solid dark/light pill, no heavy outline,
     * with flat vibrant icons sitting directly on the pill (no individual grey circles) */
    uint32_t pill_bg = dark ? makeRGBA(45, 45, 49, 250) : makeRGBA(255, 255, 255, 252);

    renderSoftShadowF(PILL_X, PILL_Y + 5.0f, PILL_W, PILL_H, PILL_H * 0.5f, 16.0f, dark ? 105 : 38);
    renderRoundedRectF(PILL_X, PILL_Y, PILL_W, PILL_H, PILL_H * 0.5f, pill_bg);

    /* Vibrant Switch 2 icon colors directly on the pill surface */
    uint32_t icon_colors[PILL_COUNT] = {
        makeRGBA(42, 206, 232, 255),  /* Netloader: Switch 2 Cyan */
        makeRGBA(248, 178, 38, 255),  /* Favorites: Warm Gold */
        makeRGBA(238, 66, 74, 255),   /* All Software: Switch 2 Coral-Red */
        makeRGBA(146, 114, 255, 255), /* Themes: Soft Violet */
        dark ? makeRGBA(212, 214, 222, 255) : makeRGBA(82, 86, 96, 255), /* Settings: Gear */
        dark ? makeRGBA(212, 214, 222, 255) : makeRGBA(82, 86, 96, 255), /* Exit: Power */
    };
    static const char *kPillLabels[PILL_COUNT] = {
        "Netloader",
        "Favorites",
        "All Software",
        "Themes",
        "Settings",
        "Exit",
    };

    for (int i = 0; i < PILL_COUNT; i++) {
        float cx = pillButtonCenterX(i);
        float cy = PILL_CENTER_Y;
        bool focused = (s_focus == S2Focus_Pill && s_pill_idx == i);
        float scale = focused ? (1.0f + 0.10f * s_pill_pop) : 1.0f;
        float r = (PILL_BTN_RADIUS + 4.0f) * scale;

        /* Subtle highlight disc only when focused */
        if (focused) {
            uint32_t foc_bg = dark ? makeRGBA(58, 60, 68, 255) : makeRGBA(240, 244, 250, 255);
            renderCircleF(cx, cy, r, foc_bg);
            renderSwitch2CircleFocusRingF(cx, cy, r + 3.5f, 4.0f, ringTimer(),
                                          themeCurrent.highlightColor.abgr,
                                          themeCurrent.highlightGradientEdgeColor.abgr);
        }

        uint32_t hole_bg = focused
            ? (dark ? makeRGBA(58, 60, 68, 255) : makeRGBA(240, 244, 250, 255))
            : pill_bg;
        drawPillIconSymbol(i, cx, cy, scale, icon_colors[i], hole_bg);

        /* Active state indicator dot for toggleable buttons */
        bool is_toggled = (i == PILL_FAVORITES && s_favorites_only) ||
                          (i == PILL_ALL_SOFTWARE && s_view == S2View_Grid) ||
                          (i == PILL_THEMES && menuIsThemeMenuActive());
        if (is_toggled) {
            renderCircleF(cx, PILL_Y + PILL_H - 7.0f, 3.5f,
                          dark ? makeRGBA(78, 224, 245, 255) : makeRGBA(0, 178, 210, 255));
        }

        if (focused) {
            /* Authentic Switch 2: cyan label appears BELOW the pill (like "Nintendo eShop") */
            const char *lbl = kPillLabels[i];
            uint32_t lw = 0, lh = 0;
            GetTextDimensions(interuiregular18, lbl, &lw, &lh);
            float lx = cx - (float)lw * 0.5f;
            float ly = PILL_Y + PILL_H + 28.0f;
            color_t lbl_col = dark ? MakeColor(78, 224, 245, 255) : MakeColor(0, 168, 204, 255);
            DrawText(interuiregular18, (uint32_t)lx, (uint32_t)ly, lbl_col, lbl);
        }
    }
}

static void drawBottomCommandStrip(menu_s *menu) {
    bool dark = isDarkTheme();
    color_t txt = themeCurrent.textColor;
    color_t muted = makeColorMuted(txt, dark ? 145 : 140);

    /* Authentic Switch 2 HOME has NO bottom divider line.
     * Bottom-left: Switch 2 Joy-Con silhouette with P1 green LED + subtle app count */
    float jx = 50.0f, jy = 660.0f;
    uint32_t jc_col = dark ? makeRGBA(225, 227, 232, 255) : makeRGBA(62, 65, 74, 255);
    uint32_t led_on = makeRGBA(130, 235, 65, 255);
    uint32_t led_off = dark ? makeRGBA(76, 78, 86, 255) : makeRGBA(185, 188, 196, 255);
    /* 4 tiny vertical P1..P4 LED dots */
    renderRectF(jx, jy + 2.0f, 4.0f, 4.0f, led_on);
    renderRectF(jx, jy + 9.0f, 4.0f, 4.0f, led_off);
    renderRectF(jx, jy + 16.0f, 4.0f, 4.0f, led_off);
    renderRectF(jx, jy + 23.0f, 4.0f, 4.0f, led_off);
    /* Left & Right Joy-Con pair */
    renderRoundedRectF(jx + 12.0f, jy, 12.0f, 30.0f, 5.5f, jc_col);
    renderRoundedRectF(jx + 27.0f, jy, 12.0f, 30.0f, 5.5f, jc_col);
    uint32_t bg_cut = themeCurrent.backgroundColor.abgr | 0xff000000u;
    renderCircleF(jx + 18.0f, jy + 9.0f, 2.5f, bg_cut);
    renderCircleF(jx + 33.0f, jy + 19.0f, 2.5f, bg_cut);

    /* Subtle count next to controller icon */
    char infoBuf[PATH_MAX + 64];
    if (menuIsThemeMenuActive()) {
        snprintf(infoBuf, sizeof(infoBuf), "Themes (%d)", s_visible_count);
    } else if (s_favorites_only) {
        snprintf(infoBuf, sizeof(infoBuf), "Favorites (%d)", s_visible_count);
    } else {
        snprintf(infoBuf, sizeof(infoBuf), "%d apps", s_visible_count);
    }
    DrawTextTruncate(interuiregular14, 104, 681, muted, infoBuf, 400, "...");

    /* Right: clean Switch 2 button prompts */
    float rx = 1232.0f;
    const uint32_t y_txt = 682;

    /* Helper macro to draw right-aligned "[Glyph] Label" prompt */
    #define DRAW_PROMPT(glyph, label) do { \
        uint32_t lw_ = 0, lh_ = 0, gw_ = 0, gh_ = 0; \
        GetTextDimensions(interuiregular18, (label), &lw_, &lh_); \
        GetTextDimensions(interuiregular18, (glyph), &gw_, &gh_); \
        rx -= (float)lw_; \
        DrawText(interuiregular18, (uint32_t)rx, y_txt, txt, (label)); \
        rx -= ((float)gw_ + 6.0f); \
        DrawText(interuiregular18, (uint32_t)rx, y_txt, txt, (glyph)); \
        rx -= 26.0f; \
    } while (0)

    if (s_focus == S2Focus_Pill) {
        DRAW_PROMPT(themeCurrent.buttonAText, "Select");
        DRAW_PROMPT(themeCurrent.buttonBText, "Back");
    } else {
        if (s_visible_count > 0) {
            if (s_sel == s_visible_count) {
                DRAW_PROMPT(themeCurrent.buttonAText, "All Software");
            } else {
                menuEntry_s *me = s_visible[s_sel];
                const char *a_label = (me->type == ENTRY_TYPE_THEME) ? "Apply" :
                                      (me->type == ENTRY_TYPE_FOLDER) ? "Open" : "Launch";
                DRAW_PROMPT(themeCurrent.buttonAText, a_label);
                if (me->type != ENTRY_TYPE_THEME) {
                    DRAW_PROMPT(themeCurrent.buttonXText, me->starred ? "Unstar" : "Star");
                }
            }
        }
        bool show_back = menuIsThemeMenuActive() || s_view == S2View_Grid || s_favorites_only ||
                         (menu && strcmp(menu->dirname, "sdmc:/") != 0 && strcmp(menu->dirname, "/") != 0);
        if (show_back) {
            DRAW_PROMPT(themeCurrent.buttonBText, "Back");
        }
        if (!menuIsThemeMenuActive()) {
            DRAW_PROMPT(themeCurrent.buttonYText, "Netloader");
            DRAW_PROMPT(themeCurrent.buttonMText, "Themes");
        }
    }
    #undef DRAW_PROMPT
}

static void changeSetting(int idx, int delta) {
    switch (idx) {
    case SET_SOUNDS:
        s_cfg.ui_sounds = s_cfg.ui_sounds ? 0 : 1;
        switch2SfxSetEnabled(s_cfg.ui_sounds != 0);
        break;
    case SET_THEME:
        s_cfg.theme_mode = (s_cfg.theme_mode + delta + 3) % 3;
        switch2ApplyThemeSetting();
        break;
    case SET_RING_SPEED:
        s_cfg.ring_speed = (s_cfg.ring_speed + delta + 4) % 4;
        break;
    case SET_TRANSITIONS:
        s_cfg.transitions = (s_cfg.transitions + delta + 3) % 3;
        break;
    case SET_DEFAULT_VIEW:
        s_cfg.default_view = s_cfg.default_view ? 0 : 1;
        break;
    case SET_SORT_ORDER:
        s_cfg.sort_order = s_cfg.sort_order ? 0 : 1;
        break;
    case SET_SHOW_SUBTITLE:
        s_cfg.show_subtitle = s_cfg.show_subtitle ? 0 : 1;
        break;
    }
    switch2SettingsSave();
    switch2SfxPlay(S2Sfx_Confirm);
}

static const char *settingValueText(int idx) {
    switch (idx) {
    case SET_SOUNDS:
        return s_cfg.ui_sounds ? "On" : "Off";
    case SET_THEME:
        if (s_cfg.theme_mode == 1) return "Basic Light";
        if (s_cfg.theme_mode == 2) return "Basic Dark";
        return "Auto (System)";
    case SET_RING_SPEED:
        if (s_cfg.ring_speed == 0) return "Slow";
        if (s_cfg.ring_speed == 1) return "Normal";
        if (s_cfg.ring_speed == 2) return "Fast";
        return "Static";
    case SET_TRANSITIONS:
        if (s_cfg.transitions == 0) return "Slide & Fade";
        if (s_cfg.transitions == 1) return "Fade";
        return "Instant";
    case SET_DEFAULT_VIEW:
        return s_cfg.default_view ? "All Software" : "Carousel";
    case SET_SORT_ORDER:
        return s_cfg.sort_order ? "Alphabetical" : "Favorites First";
    case SET_SHOW_SUBTITLE:
        return s_cfg.show_subtitle ? "On" : "Off";
    default:
        return "";
    }
}

#define SETTINGS_LIST_X  388.0f
#define SETTINGS_LIST_Y  116.0f
#define SETTINGS_LIST_W  828.0f
#define SETTINGS_ROW_H   68.0f

static float transEase(void) {
    float t = s_trans_t;
    if (t >= 1.0f) return 1.0f;
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv; /* cubic ease-out */
}

static float transSlideOffset(void) {
    if (s_cfg.transitions != 0 || s_trans_t >= 1.0f)
        return 0.0f;
    return (1.0f - transEase()) * 44.0f;
}

static void triggerTransition(void) {
    s_trans_t = (s_cfg.transitions == 2) ? 1.0f : 0.0f;
}

static void drawSettingsView(void) {
    bool dark = isDarkTheme();
    color_t txt = themeCurrent.textColor;
    color_t muted = makeColorMuted(txt, dark ? 150 : 145);
    color_t accent = dark ? MakeColor(78, 224, 245, 255) : MakeColor(0, 168, 204, 255);
    uint32_t sep_col = dark ? makeRGBA(62, 64, 72, 255) : makeRGBA(212, 215, 222, 255);

    /* Top Settings header bar (authentic Switch 2 System Settings style) */
    const float bar_cy = 50.0f;
    renderSettingsIconF(74.0f, bar_cy, 18.0f, txt.abgr | 0xff000000u);
    DrawText(interuimedium20, 108, (uint32_t)(bar_cy + fontBaselineOffset(interuimedium20)), txt, "Settings");

    /* Right status: Clock + Horizontal Battery */
    float right_x = 1232.0f;
    uint32_t batteryCharge = 100;
    bool isCharging = false;
    if (powerGetDetails(&batteryCharge, &isCharging)) {
        if (batteryCharge > 100) batteryCharge = 100;
        right_x -= 40.0f;
        drawHorizontalBattery(right_x, bar_cy - 9.5f, batteryCharge, isCharging, txt, dark);
        right_x -= 22.0f;
    }
    time_t unixTime = time(NULL);
    struct tm *tmNow = localtime(&unixTime);
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", tmNow->tm_hour, tmNow->tm_min);
    uint32_t cw = 0, ch = 0;
    GetTextDimensions(interuimedium20, timeStr, &cw, &ch);
    right_x -= (float)cw;
    DrawText(interuimedium20, (uint32_t)right_x, (uint32_t)(bar_cy + fontBaselineOffset(interuimedium20)), txt, timeStr);

    /* Header & sidebar hairline dividers */
    renderRectF(42.0f, 92.0f, SCREEN_W - 84.0f, 1.0f, sep_col);
    renderRectF(344.0f, 114.0f, 1.0f, 514.0f, sep_col);

    /* Left sidebar category ("General" active with Switch 2 cyan left bar) */
    float cat_y = 126.0f;
    uint32_t cat_bg = dark ? makeRGBA(40, 42, 48, 255) : makeRGBA(246, 248, 252, 255);
    renderRoundedRectF(56.0f, cat_y, 260.0f, 56.0f, 10.0f, cat_bg);
    renderRoundedRectF(66.0f, cat_y + 12.0f, 4.5f, 32.0f, 2.25f, accent.abgr | 0xff000000u);
    DrawText(interuimedium20, 86, (uint32_t)(cat_y + 28.0f + fontBaselineOffset(interuimedium20)), accent, "General");

    DrawText(interuiregular14, 68, 550, muted, "Switch 2 HOME UI");
    DrawText(interuiregular14, 68, 572, muted, "by ImDella");
    DrawText(interuiregular14, 68, 594, muted, "Base: ouncebrew / switchbrew");
    DrawText(interuiregular14, 68, 616, muted, "Saved automatically to SD");

    /* Right settings list */
    static const char *kLabels[SET_COUNT] = {
        "UI Sounds",
        "Theme",
        "Focus Border Speed",
        "Transitions",
        "Default View",
        "Sort Order",
        "Show Author & Version",
    };

    float y_off = transSlideOffset();
    for (int i = 0; i < SET_COUNT; i++) {
        float ry = SETTINGS_LIST_Y + (float)i * SETTINGS_ROW_H + y_off;
        float rcy = ry + SETTINGS_ROW_H * 0.5f;
        bool focused = (i == s_settings_sel);

        if (i > 0)
            renderRectF(SETTINGS_LIST_X + 12.0f, ry, SETTINGS_LIST_W - 24.0f, 1.0f, sep_col);

        if (focused) {
            uint32_t row_bg = dark ? makeRGBA(42, 44, 52, 255) : makeRGBA(255, 255, 255, 255);
            renderRoundedRectF(SETTINGS_LIST_X, ry + 4.0f, SETTINGS_LIST_W, SETTINGS_ROW_H - 8.0f, 12.0f, row_bg);
            renderRoundedRectF(SETTINGS_LIST_X + 10.0f, ry + 18.0f, 4.5f, SETTINGS_ROW_H - 36.0f, 2.25f, accent.abgr | 0xff000000u);
        }

        /* Setting name on left */
        DrawText(interuiregular18, (uint32_t)(SETTINGS_LIST_X + 28.0f),
                 (uint32_t)(rcy + fontBaselineOffset(interuiregular18)),
                 focused ? txt : makeColorMuted(txt, dark ? 220 : 210), kLabels[i]);

        /* Value on right ("< Value >" when focused, clean value otherwise) */
        const char *val = settingValueText(i);
        char valBuf[64];
        if (focused)
            snprintf(valBuf, sizeof(valBuf), "\u25C2  %s  \u25B8", val);
        else
            snprintf(valBuf, sizeof(valBuf), "%s", val);

        uint32_t vw = 0, vh = 0;
        GetTextDimensions(interuiregular18, valBuf, &vw, &vh);
        color_t val_col = focused ? accent : muted;
        DrawText(interuiregular18, (uint32_t)(SETTINGS_LIST_X + SETTINGS_LIST_W - 26.0f - (float)vw),
                 (uint32_t)(rcy + fontBaselineOffset(interuiregular18)), val_col, valBuf);
    }

    /* Footer bar */
    renderRectF(42.0f, 644.0f, SCREEN_W - 84.0f, 1.0f, sep_col);
    float rx = 1232.0f;
    const uint32_t y_txt = 682;
    #define DRAW_SET_PROMPT(glyph, label) do { \
        uint32_t lw_ = 0, lh_ = 0, gw_ = 0, gh_ = 0; \
        GetTextDimensions(interuiregular18, (label), &lw_, &lh_); \
        GetTextDimensions(interuiregular18, (glyph), &gw_, &gh_); \
        rx -= (float)lw_; \
        DrawText(interuiregular18, (uint32_t)rx, y_txt, txt, (label)); \
        rx -= ((float)gw_ + 6.0f); \
        DrawText(interuiregular18, (uint32_t)rx, y_txt, txt, (glyph)); \
        rx -= 28.0f; \
    } while (0)

    DRAW_SET_PROMPT(themeCurrent.buttonAText, "Change");
    DRAW_SET_PROMPT(themeCurrent.buttonBText, "Back");
    #undef DRAW_SET_PROMPT
}

static void updateAnimations(void) {
    /* Trigger elastic pop spring whenever focused item changes */
    if (s_sel != s_last_sel || (s_focus == S2Focus_Apps && s_last_focus != S2Focus_Apps)) {
        s_app_pop = 0.0f;
        s_app_pop_vel = 0.28f;
        s_last_sel = s_sel;
    }
    if (s_pill_idx != s_last_pill || (s_focus == S2Focus_Pill && s_last_focus != S2Focus_Pill)) {
        s_pill_pop = 0.0f;
        s_pill_pop_vel = 0.30f;
        s_last_pill = s_pill_idx;
    }
    s_last_focus = s_focus;

    /* Damped spring integration (60fps) -> snappy Switch 2 bounce */
    s_app_pop_vel += (1.0f - s_app_pop) * 0.24f;
    s_app_pop_vel *= 0.62f;
    s_app_pop += s_app_pop_vel;

    s_pill_pop_vel += (1.0f - s_pill_pop) * 0.26f;
    s_pill_pop_vel *= 0.60f;
    s_pill_pop += s_pill_pop_vel;
    /* View transition animation (~240ms cubic ease-out at 60fps) */
    if (s_view != s_last_view) {
        s_last_view = s_view;
        triggerTransition();
    }
    if (s_trans_t < 1.0f) {
        s_trans_t += 0.068f;
        if (s_trans_t > 1.0f) s_trans_t = 1.0f;
    }
}

/* --- Public Entry Points -------------------------------------------------- */

static void drawTransitionVeil(void) {
    if (s_cfg.transitions == 2 || s_trans_t >= 1.0f)
        return;
    float alpha_f = (1.0f - transEase()) * 215.0f;
    uint8_t a = (uint8_t)alpha_f;
    if (a > 0) {
        uint32_t bg = (themeCurrent.backgroundColor.abgr & 0x00ffffffu) | ((uint32_t)a << 24);
        renderRectF(0.0f, 0.0f, SCREEN_W, SCREEN_H, bg);
    }
}

void switch2MenuDraw(void) {
    menu_s *menu = menuGetCurrent();
    rebuildVisibleList(menu);
    updateAnimations();

    /* Clean Switch 2 background */
    renderRectF(0.0f, 0.0f, SCREEN_W, SCREEN_H, themeCurrent.backgroundColor.abgr | 0xff000000u);

    /* Optional custom theme background image (if user loaded a .romfs theme with one) */
    ThemeLayoutObject *bgObj = &themeCurrent.layoutObjects[ThemeLayoutId_BackgroundImage];
    assetsDataEntry *bgData = NULL;
    assetsGetData(AssetId_background_image, &bgData);
    if (bgObj->visible && bgData && bgData->buffer) {
        enum RenderImageFormat fmt = (bgData->imageMode == IMAGE_MODE_RGB24) ? RenderImageFormat_Rgb24 : RenderImageFormat_Rgba32;
        renderImageScaledF(bgData->buffer, bgData->imageSize[0], bgData->imageSize[1],
                           0.0f, 0.0f, SCREEN_W, SCREEN_H, fmt, 0xffffffffu);
    }

    if (s_view == S2View_Settings) {
        drawSettingsView();
        drawTransitionVeil();
        return;
    }

    if (!menuIsNetloaderActive()) {
        if (s_view == S2View_Grid && !menuIsThemeMenuActive())
            drawGridView();
        else
            drawCarouselView();

        drawTopBar(menu);
        drawBottomPillBar();
    } else {
        drawTopBar(menu);
    }

    drawBottomCommandStrip(menu);
    drawTransitionVeil();
}

bool switch2MenuUpdate(u64 down) {
    menu_s *menu = menuGetCurrent();
    rebuildVisibleList(menu);

    if (s_exit_requested) {
        s_exit_requested = false;
        return true;
    }

    /* Full-screen Switch 2 Settings navigation */
    if (s_view == S2View_Settings) {
        if (down & HidNpadButton_Plus)
            return true;
        if (down & HidNpadButton_B) {
            switch2SfxPlay(S2Sfx_Back);
            s_view = s_prev_view;
            s_focus = S2Focus_Pill;
            return false;
        }
        if (down & HidNpadButton_AnyUp) {
            if (s_settings_sel > 0) {
                s_settings_sel--;
                switch2SfxPlay(S2Sfx_Pill);
            }
        } else if (down & HidNpadButton_AnyDown) {
            if (s_settings_sel < SET_COUNT - 1) {
                s_settings_sel++;
                switch2SfxPlay(S2Sfx_Pill);
            }
        } else if (down & (HidNpadButton_AnyRight | HidNpadButton_A)) {
            changeSetting(s_settings_sel, +1);
        } else if (down & HidNpadButton_AnyLeft) {
            changeSetting(s_settings_sel, -1);
        }
        return false;
    }

    /* Confirmation / message box handling */
    if (menuIsConfirmMsgBoxOpen()) {
        if (down & HidNpadButton_A) { switch2SfxPlay(S2Sfx_Confirm); menuHandleAButton(); }
        else if (down & HidNpadButton_B) { switch2SfxPlay(S2Sfx_Back); launchMenuBackTask(); }
        else if (down & HidNpadButton_Plus) return true;
        return false;
    }
    if (menuIsMsgBoxOpen() && !menuIsNetloaderActive()) {
        if (down & (HidNpadButton_A | HidNpadButton_B)) { switch2SfxPlay(S2Sfx_Back); menuCloseMsgBox(); }
        else if (down & HidNpadButton_Plus) return true;
        return false;
    }

    if (down & HidNpadButton_Plus)
        return true;

    if (down & HidNpadButton_Y) {
        if (!menuIsThemeMenuActive()) {
            switch2SfxPlay(S2Sfx_Confirm);
            launchMenuNetloaderTask();
        }
        return false;
    }
    if (down & HidNpadButton_Minus) {
        triggerPillAction(PILL_THEMES);
        return false;
    }

    /* Pill bar focus navigation */
    if (s_focus == S2Focus_Pill) {
        if (down & HidNpadButton_AnyLeft) {
            if (s_pill_idx > 0) { s_pill_idx--; switch2SfxPlay(S2Sfx_Pill); }
        } else if (down & HidNpadButton_AnyRight) {
            if (s_pill_idx < PILL_COUNT - 1) { s_pill_idx++; switch2SfxPlay(S2Sfx_Pill); }
        } else if (down & (HidNpadButton_AnyUp | HidNpadButton_B)) {
            if (s_visible_count > 0) {
                s_focus = S2Focus_Apps;
                switch2SfxPlay(S2Sfx_Move);
            }
        } else if (down & HidNpadButton_A) {
            triggerPillAction(s_pill_idx);
        }
        return false;
    }

    /* App list focus navigation */
    if (down & HidNpadButton_B) {
        switch2SfxPlay(S2Sfx_Back);
        if (s_view == S2View_Grid) {
            s_view = S2View_Carousel;
        } else if (s_favorites_only && !menuIsThemeMenuActive()) {
            s_favorites_only = false;
            syncSelectionFromMenu(menu);
        } else {
            launchMenuBackTask();
            s_sel = 0;
            s_scroll_x = 0.0f;
            s_grid_scroll_y = 0.0f;
            syncSelectionFromMenu(menuGetCurrent());
        }
        return false;
    }

    if (down & HidNpadButton_A) {
        if (s_visible_count > 0) {
            switch2SfxPlay(S2Sfx_Confirm);
            if (s_sel == s_visible_count && hasEndAllSoftwareTile()) {
                s_view = S2View_Grid;
                s_sel = s_visible_count - 1;
            } else if (s_sel < s_visible_count) {
                menu->curEntry = s_visible_indices[s_sel];
                launchMenuEntryTask(s_visible[s_sel]);
                syncSelectionFromMenu(menuGetCurrent());
            }
        }
        return false;
    }

    if (down & HidNpadButton_X) {
        if (s_visible_count > 0 && s_sel < s_visible_count && !menuIsThemeMenuActive()) {
            switch2SfxPlay(S2Sfx_Star);
            menuEntry_s *target = s_visible[s_sel];
            toggleStarState(target);
            syncSelectionFromMenu(menuGetCurrent());
        }
        return false;
    }

    int total_slots = s_visible_count + (hasEndAllSoftwareTile() ? 1 : 0);
    int prev_sel = s_sel;
    S2FocusZone prev_focus = s_focus;

    if (s_view == S2View_Carousel) {
        if (down & HidNpadButton_AnyLeft) {
            if (s_sel > 0) s_sel--;
        } else if (down & HidNpadButton_AnyRight) {
            if (s_sel < total_slots - 1) s_sel++;
        } else if (down & HidNpadButton_ZL) {
            s_sel -= 4;
            if (s_sel < 0) s_sel = 0;
        } else if (down & HidNpadButton_ZR) {
            s_sel += 4;
            if (s_sel >= total_slots) s_sel = total_slots > 0 ? total_slots - 1 : 0;
        } else if (down & HidNpadButton_AnyDown) {
            s_focus = S2Focus_Pill;
        }
    } else {
        /* 5-column All Software Grid navigation */
        if (down & HidNpadButton_AnyLeft) {
            if (s_sel > 0) s_sel--;
        } else if (down & HidNpadButton_AnyRight) {
            if (s_sel < s_visible_count - 1) s_sel++;
        } else if (down & HidNpadButton_AnyUp) {
            if (s_sel >= GRID_COLS) s_sel -= GRID_COLS;
        } else if (down & HidNpadButton_AnyDown) {
            if (s_sel + GRID_COLS < s_visible_count)
                s_sel += GRID_COLS;
            else
                s_focus = S2Focus_Pill;
        } else if (down & HidNpadButton_ZL) {
            s_sel -= GRID_COLS * 2;
            if (s_sel < 0) s_sel = 0;
        } else if (down & HidNpadButton_ZR) {
            s_sel += GRID_COLS * 2;
            if (s_sel >= s_visible_count) s_sel = s_visible_count > 0 ? s_visible_count - 1 : 0;
        }
    }

    if (s_focus != prev_focus)
        switch2SfxPlay(S2Sfx_Pill);
    else if (s_sel != prev_sel)
        switch2SfxPlay(S2Sfx_Move);

    if (s_visible_count > 0 && s_sel < s_visible_count)
        menu->curEntry = s_visible_indices[s_sel];

    return false;
}

void switch2HandleTouch(menu_s *menu) {
    HidTouchScreenState touch = {0};
    hidGetTouchScreenStates(&touch, 1);

    if (touch.count >= 1) {
        int tx = (int)touch.touches[0].x;
        int ty = (int)touch.touches[0].y;
        if (!s_touch_active) {
            s_touch_active = true;
            s_touch_is_tap = true;
            s_touch_start_x = s_touch_prev_x = tx;
            s_touch_start_y = s_touch_prev_y = ty;
            s_touch_vel_x = 0.0f;
        } else {
            int dx = tx - s_touch_prev_x;
            int dy = ty - s_touch_prev_y;
            if (abs(tx - s_touch_start_x) > 18 || abs(ty - s_touch_start_y) > 18)
                s_touch_is_tap = false;
            if (!s_touch_is_tap && !menuIsMsgBoxOpen() && s_view == S2View_Carousel) {
                s_scroll_x -= (float)dx;
                s_touch_vel_x = (float)dx;
                if (s_scroll_x < 0.0f) s_scroll_x = 0.0f;
            }
            (void)dy;
            s_touch_prev_x = tx;
            s_touch_prev_y = ty;
        }
    } else if (s_touch_active) {
        int x1 = s_touch_start_x;
        int y1 = s_touch_start_y;
        bool was_tap = s_touch_is_tap;
        s_touch_active = false;

        if (!was_tap || menuIsNetloaderActive())
            return;

        /* Settings view tap handling */
        if (s_view == S2View_Settings) {
            if (x1 >= (int)SETTINGS_LIST_X && x1 <= (int)(SETTINGS_LIST_X + SETTINGS_LIST_W)) {
                for (int i = 0; i < SET_COUNT; i++) {
                    float ry = SETTINGS_LIST_Y + (float)i * SETTINGS_ROW_H;
                    if (y1 >= (int)ry && y1 <= (int)(ry + SETTINGS_ROW_H)) {
                        s_settings_sel = i;
                        changeSetting(i, +1);
                        return;
                    }
                }
            }
            if (y1 >= 644 || (x1 < 344 && y1 < 92)) {
                switch2SfxPlay(S2Sfx_Back);
                s_view = s_prev_view;
                s_focus = S2Focus_Pill;
            }
            return;
        }

        /* Message box tap */
        if (menuIsMsgBoxOpen()) {
            ThemeLayoutObject *sep = &themeCurrent.layoutObjects[ThemeLayoutId_MsgBoxSeparator];
            MessageBox mb = menuGetCurrentMsgBox();
            int sx = 1280 / 2 - mb.width / 2;
            int sy = (720 / 2 - mb.height / 2) + mb.height + sep->posStart[1];
            int ex = sx + mb.width;
            int ey = (720 / 2 - mb.height / 2) + mb.height;
            if (x1 > sx && x1 < ex && y1 > sy && y1 < ey) {
                if (menuIsConfirmMsgBoxOpen() && x1 < sx + (int)mb.width / 2)
                    menuConfirmMsgBox();
                else
                    menuCloseMsgBox();
            }
            return;
        }

        /* Bottom Pill Bar tap */
        if (y1 >= (int)(PILL_Y - 10) && y1 <= (int)(PILL_Y + PILL_H + 10) &&
            x1 >= (int)(PILL_X - 10) && x1 <= (int)(PILL_X + PILL_W + 10)) {
            for (int i = 0; i < PILL_COUNT; i++) {
                float cx = pillButtonCenterX(i);
                float dx = (float)x1 - cx;
                float dy = (float)y1 - PILL_CENTER_Y;
                if (dx * dx + dy * dy <= (PILL_BTN_RADIUS + 10.0f) * (PILL_BTN_RADIUS + 10.0f)) {
                    s_pill_idx = i;
                    s_focus = S2Focus_Pill;
                    triggerPillAction(i);
                    return;
                }
            }
        }

        /* Carousel / Grid tile tap */
        rebuildVisibleList(menu);
        if (s_view == S2View_Carousel) {
            if (y1 >= (int)CAROUSEL_TILE_Y && y1 <= (int)(CAROUSEL_TILE_Y + CAROUSEL_TILE_SZ)) {
                int total_slots = s_visible_count + (hasEndAllSoftwareTile() ? 1 : 0);
                for (int i = 0; i < total_slots; i++) {
                    float tx = CAROUSEL_START_X + (float)i * CAROUSEL_STRIDE - s_scroll_x;
                    if (x1 >= (int)tx && x1 <= (int)(tx + CAROUSEL_TILE_SZ)) {
                        if (s_focus == S2Focus_Apps && s_sel == i) {
                            if (i == s_visible_count) {
                                s_view = S2View_Grid;
                                s_sel = s_visible_count - 1;
                            } else {
                                menu->curEntry = s_visible_indices[i];
                                launchMenuEntryTask(s_visible[i]);
                                syncSelectionFromMenu(menuGetCurrent());
                            }
                        } else {
                            s_focus = S2Focus_Apps;
                            s_sel = i;
                            if (i < s_visible_count)
                                menu->curEntry = s_visible_indices[i];
                        }
                        return;
                    }
                }
            }
        } else {
            if (y1 >= (int)GRID_CLIP_TOP && y1 <= (int)GRID_CLIP_BOTTOM) {
                for (int i = 0; i < s_visible_count; i++) {
                    int col = i % GRID_COLS;
                    int row = i / GRID_COLS;
                    float tx = GRID_START_X + (float)col * GRID_STRIDE_X;
                    float ty = GRID_START_Y + (float)row * GRID_STRIDE_Y - s_grid_scroll_y;
                    if (x1 >= (int)tx && x1 <= (int)(tx + GRID_TILE_SZ) &&
                        y1 >= (int)ty && y1 <= (int)(ty + GRID_TILE_SZ)) {
                        if (s_focus == S2Focus_Apps && s_sel == i) {
                            menu->curEntry = s_visible_indices[i];
                            launchMenuEntryTask(s_visible[i]);
                            syncSelectionFromMenu(menuGetCurrent());
                        } else {
                            s_focus = S2Focus_Apps;
                            s_sel = i;
                            menu->curEntry = s_visible_indices[i];
                        }
                        return;
                    }
                }
            }
        }
    }
}
