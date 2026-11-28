/* SPDX-License-Identifier: ISC
 * Switch 2 HOME Menu UI for ounce-hbmenu (v2.0).
 * Features:
 * - Authentic Switch 2 HOME Carousel, All Software Grid, and Floating Pill Bar
 * - Sidebar System Settings (General / Appearance / Audio) with live previews
 * - Custom JPG/PNG Wallpapers with dimming, Focus/Accent Color Presets + Custom RGB
 * - Streaming BGM (WAV/MP3/OGG) & ZIP Sound Packs with launch fade-out + zoom animation
 */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <libconfig.h>
#include "switch2_ui.h"
#include "switch2_sfx.h"
#include "switch2_wallpaper.h"
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

typedef enum {
    S2Cat_General = 0,
    S2Cat_Appearance = 1,
    S2Cat_Audio = 2,
    S2Cat_Count = 3,
} S2SettingsCat;

/* Setting item IDs */
enum {
    /* General */
    SET_DEFAULT_VIEW = 0,
    SET_SORT_ORDER,
    SET_SHOW_SUBTITLE,
    SET_BATTERY_PCT,
    SET_CLOCK_FORMAT,
    SET_TRANSITIONS,
    SET_RESET_DEFAULTS,
    /* Appearance */
    SET_THEME,
    SET_WALLPAPER,
    SET_WALLPAPER_DIM,
    SET_RING_SPEED,
    SET_ACCENT_PRESET,
    SET_CUSTOM_R,
    SET_CUSTOM_G,
    SET_CUSTOM_B,
    /* Audio */
    SET_SOUNDS,
    SET_SFX_PACK,
    SET_SFX_VOL,
    SET_BGM_TRACK,
    SET_BGM_VOL,
};

enum {
    ACCENT_IRIDESCENT = 0,
    ACCENT_NEON_CYAN  = 1,
    ACCENT_SWITCH_RED = 2,
    ACCENT_VIOLET     = 3,
    ACCENT_GOLD       = 4,
    ACCENT_EMERALD    = 5,
    ACCENT_CUSTOM_RGB = 6,
    ACCENT_COUNT      = 7,
};

#define MAX_MEDIA_FILES 64
#define MEDIA_NAME_MAX  128

typedef struct {
    int ui_sounds;          /* 0 = Off, 1 = On */
    int theme_mode;         /* 0 = Auto, 1 = Light, 2 = Dark */
    int ring_speed;         /* 0 = Slow, 1 = Normal, 2 = Fast, 3 = Static */
    int transitions;        /* 0 = Slide & Fade, 1 = Fade, 2 = Instant */
    int default_view;       /* 0 = Carousel, 1 = All Software */
    int sort_order;         /* 0 = Favorites First, 1 = Alphabetical */
    int show_subtitle;      /* 0 = Off, 1 = On */
    int show_battery_pct;   /* 0 = Off, 1 = On */
    int clock_12h;          /* 0 = 24-Hour, 1 = 12-Hour */
    int wallpaper_dim;      /* 0..80 (step 10) */
    int accent_preset;      /* 0..ACCENT_COUNT-1 */
    int custom_r;           /* 0..255 */
    int custom_g;           /* 0..255 */
    int custom_b;           /* 0..255 */
    int sfx_volume;         /* 0..100 */
    int bgm_volume;         /* 0..100 */
    char wallpaper[MEDIA_NAME_MAX]; /* "" = None */
    char sfx_pack[MEDIA_NAME_MAX];  /* "" = Built-in */
    char bgm_track[MEDIA_NAME_MAX]; /* "" = Off, "__PACK__" = Pack BGM, "__RANDOM__" = Random, else filename */
} Switch2Settings;

static const Switch2Settings kDefaultCfg = {
    .ui_sounds = 1,
    .theme_mode = 0,
    .ring_speed = 1,
    .transitions = 0,
    .default_view = 0,
    .sort_order = 0,
    .show_subtitle = 1,
    .show_battery_pct = 0,
    .clock_12h = 0,
    .wallpaper_dim = 30,
    .accent_preset = ACCENT_IRIDESCENT,
    .custom_r = 78,
    .custom_g = 224,
    .custom_b = 245,
    .sfx_volume = 100,
    .bgm_volume = 50,
    .wallpaper = "",
    .sfx_pack = "",
    .bgm_track = "",
};

static Switch2Settings s_cfg;
static ThemePreset s_system_preset = THEME_PRESET_DARK;
static S2ViewMode s_prev_view = S2View_Carousel;
static S2ViewMode s_last_view = S2View_Carousel;
static float s_trans_t = 1.0f;

/* Settings UI state */
static S2SettingsCat s_settings_cat = S2Cat_General;
static int s_settings_sel = 0;
static int s_settings_scroll = 0;
static bool s_reset_armed = false;

/* Scanned media lists on SD */
static char s_wallpapers[MAX_MEDIA_FILES][MEDIA_NAME_MAX];
static int s_wallpaper_count = 0;
static char s_sfx_packs[MAX_MEDIA_FILES][MEDIA_NAME_MAX];
static int s_sfx_pack_count = 0;
static char s_bgm_tracks[MAX_MEDIA_FILES][MEDIA_NAME_MAX];
static int s_bgm_track_count = 0;

/* Switch 2 launch animation state (~200ms zoom/fade + BGM fade-out) */
static menuEntry_s *s_launching_entry = NULL;
static float s_launch_t = 0.0f;
static bool s_launch_blackout = false;

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

/* Visible filtered entry list */
static menuEntry_s *s_visible[MAX_VISIBLE_ENTRIES];
static int s_visible_indices[MAX_VISIBLE_ENTRIES];
static int s_visible_count = 0;
static int s_sel = 0;

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

/* --- Color & Utility Helpers ---------------------------------------------- */

static bool isDarkTheme(void) {
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
    case 0:  return (float)menuTimer * 0.45f;
    case 1:  return (float)menuTimer * 1.0f;
    case 2:  return (float)menuTimer * 2.0f;
    case 3:  return 0.0f;
    default: return (float)menuTimer;
    }
}

/* Returns primary & secondary ring colors (0,0 = Switch 2 Iridescent) */
static void getFocusRingColors(uint32_t *c1, uint32_t *c2) {
    switch (s_cfg.accent_preset) {
    case ACCENT_NEON_CYAN:
        *c1 = makeRGBA(0, 212, 245, 255);
        *c2 = makeRGBA(130, 252, 255, 255);
        break;
    case ACCENT_SWITCH_RED:
        *c1 = makeRGBA(230, 36, 56, 255);
        *c2 = makeRGBA(255, 130, 110, 255);
        break;
    case ACCENT_VIOLET:
        *c1 = makeRGBA(136, 92, 255, 255);
        *c2 = makeRGBA(215, 150, 255, 255);
        break;
    case ACCENT_GOLD:
        *c1 = makeRGBA(240, 170, 24, 255);
        *c2 = makeRGBA(255, 232, 128, 255);
        break;
    case ACCENT_EMERALD:
        *c1 = makeRGBA(34, 206, 138, 255);
        *c2 = makeRGBA(140, 255, 205, 255);
        break;
    case ACCENT_CUSTOM_RGB: {
        uint8_t r = (uint8_t)s_cfg.custom_r;
        uint8_t g = (uint8_t)s_cfg.custom_g;
        uint8_t b = (uint8_t)s_cfg.custom_b;
        uint8_t r2 = (uint8_t)(r + (255 - r) * 45 / 100);
        uint8_t g2 = (uint8_t)(g + (255 - g) * 45 / 100);
        uint8_t b2 = (uint8_t)(b + (255 - b) * 45 / 100);
        *c1 = makeRGBA(r, g, b, 255);
        *c2 = makeRGBA(r2, g2, b2, 255);
        break;
    }
    case ACCENT_IRIDESCENT:
    default:
        *c1 = 0;
        *c2 = 0;
        break;
    }
}

static color_t getAccentColor(bool dark) {
    switch (s_cfg.accent_preset) {
    case ACCENT_NEON_CYAN:  return dark ? MakeColor(78, 224, 245, 255) : MakeColor(0, 168, 204, 255);
    case ACCENT_SWITCH_RED: return dark ? MakeColor(255, 92, 108, 255) : MakeColor(218, 32, 52, 255);
    case ACCENT_VIOLET:     return dark ? MakeColor(172, 136, 255, 255) : MakeColor(118, 72, 224, 255);
    case ACCENT_GOLD:       return dark ? MakeColor(255, 206, 68, 255) : MakeColor(196, 132, 0, 255);
    case ACCENT_EMERALD:    return dark ? MakeColor(72, 232, 164, 255) : MakeColor(16, 158, 102, 255);
    case ACCENT_CUSTOM_RGB: return MakeColor((u8)s_cfg.custom_r, (u8)s_cfg.custom_g, (u8)s_cfg.custom_b, 255);
    case ACCENT_IRIDESCENT:
    default:                return dark ? MakeColor(78, 224, 245, 255) : MakeColor(0, 168, 204, 255);
    }
}

static void formatClockString(char *buf, size_t sz) {
    time_t unixTime = time(NULL);
    struct tm *tmNow = localtime(&unixTime);
    if (!tmNow) {
        snprintf(buf, sz, "--:--");
        return;
    }
    if (s_cfg.clock_12h) {
        int h = tmNow->tm_hour % 12;
        if (h == 0) h = 12;
        const char *ampm = (tmNow->tm_hour >= 12) ? "PM" : "AM";
        snprintf(buf, sz, "%d:%02d %s", h, tmNow->tm_min, ampm);
    } else {
        snprintf(buf, sz, "%02d:%02d", tmNow->tm_hour, tmNow->tm_min);
    }
}

/* --- SD Folder Setup & Media Scanner -------------------------------------- */

static void getConfigBaseDir(char *out, size_t out_size) {
    snprintf(out, out_size, "%s/config/nx-hbmenu", menuGetRootBasePath());
}

static void ensureMediaDirs(void) {
    char p[PATH_MAX + 64];
    snprintf(p, sizeof(p), "%s/config", menuGetRootBasePath());
    mkdir(p, 0777);
    getConfigBaseDir(p, sizeof(p));
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/config/nx-hbmenu/wallpapers", menuGetRootBasePath());
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/config/nx-hbmenu/bgm", menuGetRootBasePath());
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/config/nx-hbmenu/sfx", menuGetRootBasePath());
    mkdir(p, 0777);
}

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

static int cmpMediaName(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static int scanMediaDir(const char *subdir, const char *const *exts, int ext_count,
                        char out_list[MAX_MEDIA_FILES][MEDIA_NAME_MAX]) {
    char dirpath[PATH_MAX + 64];
    snprintf(dirpath, sizeof(dirpath), "%s/config/nx-hbmenu/%s", menuGetRootBasePath(), subdir);
    DIR *d = opendir(dirpath);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < MAX_MEDIA_FILES) {
        if (ent->d_name[0] == '.')
            continue;
        bool match = false;
        for (int i = 0; i < ext_count; i++) {
            if (hasExtCI(ent->d_name, exts[i])) {
                match = true;
                break;
            }
        }
        if (!match)
            continue;
        strncpy(out_list[count], ent->d_name, MEDIA_NAME_MAX - 1);
        out_list[count][MEDIA_NAME_MAX - 1] = '\0';
        count++;
    }
    closedir(d);

    if (count > 1)
        qsort(out_list, (size_t)count, MEDIA_NAME_MAX, cmpMediaName);
    return count;
}

static void rescanMediaLists(void) {
    static const char *const kImgExts[] = { ".jpg", ".jpeg", ".png" };
    static const char *const kZipExts[] = { ".zip" };
    static const char *const kBgmExts[] = { ".wav", ".mp3", ".ogg" };

    s_wallpaper_count = scanMediaDir("wallpapers", kImgExts, 3, s_wallpapers);
    s_sfx_pack_count  = scanMediaDir("sfx", kZipExts, 1, s_sfx_packs);
    s_bgm_track_count = scanMediaDir("bgm", kBgmExts, 3, s_bgm_tracks);
}

/* --- Settings Persistence & Application ----------------------------------- */

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

static void switch2ApplyWallpaper(void) {
    if (!s_cfg.wallpaper[0]) {
        switch2WallpaperUnload();
        return;
    }
    char full[PATH_MAX + 128];
    snprintf(full, sizeof(full), "%s/config/nx-hbmenu/wallpapers/%s", menuGetRootBasePath(), s_cfg.wallpaper);
    if (!switch2WallpaperLoad(full)) {
        s_cfg.wallpaper[0] = '\0';
    }
}

static void switch2ApplyBgm(void) {
    switch2BgmSetVolume(s_cfg.bgm_volume);
    if (!s_cfg.bgm_track[0]) {
        switch2BgmStop();
        return;
    }
    if (strcmp(s_cfg.bgm_track, "__PACK__") == 0) {
        if (s_cfg.sfx_pack[0] && switch2SfxPackHasBgm()) {
            char full[PATH_MAX + 128];
            snprintf(full, sizeof(full), "%s/config/nx-hbmenu/sfx/%s", menuGetRootBasePath(), s_cfg.sfx_pack);
            switch2BgmPlayPack(full);
        } else {
            switch2BgmStop();
        }
        return;
    }
    if (strcmp(s_cfg.bgm_track, "__RANDOM__") == 0) {
        if (s_bgm_track_count > 0) {
            int pick = rand() % s_bgm_track_count;
            char full[PATH_MAX + 128];
            snprintf(full, sizeof(full), "%s/config/nx-hbmenu/bgm/%s", menuGetRootBasePath(), s_bgm_tracks[pick]);
            switch2BgmPlayFile(full);
        } else {
            switch2BgmStop();
        }
        return;
    }
    char full[PATH_MAX + 128];
    snprintf(full, sizeof(full), "%s/config/nx-hbmenu/bgm/%s", menuGetRootBasePath(), s_cfg.bgm_track);
    if (!switch2BgmPlayFile(full)) {
        switch2BgmStop();
    }
}

static void switch2ApplySfxPack(void) {
    switch2SfxSetEnabled(s_cfg.ui_sounds != 0);
    switch2SfxSetVolume(s_cfg.sfx_volume);
    if (!s_cfg.sfx_pack[0]) {
        switch2SfxLoadPack(NULL);
        if (strcmp(s_cfg.bgm_track, "__PACK__") == 0) {
            s_cfg.bgm_track[0] = '\0';
            switch2BgmStop();
        }
        return;
    }
    char full[PATH_MAX + 128];
    snprintf(full, sizeof(full), "%s/config/nx-hbmenu/sfx/%s", menuGetRootBasePath(), s_cfg.sfx_pack);
    if (!switch2SfxLoadPack(full)) {
        s_cfg.sfx_pack[0] = '\0';
        switch2SfxLoadPack(NULL);
    }
    if (strcmp(s_cfg.bgm_track, "__PACK__") == 0) {
        switch2ApplyBgm();
    }
}

static void switch2SettingsSave(void) {
    config_t cfg;
    config_init(&cfg);
    config_setting_t *root = config_root_setting(&cfg);
    config_setting_t *s;

    #define SAVE_INT(k, v) do { s = config_setting_add(root, (k), CONFIG_TYPE_INT); if (s) config_setting_set_int(s, (v)); } while (0)
    #define SAVE_STR(k, v) do { s = config_setting_add(root, (k), CONFIG_TYPE_STRING); if (s) config_setting_set_string(s, (v)); } while (0)

    SAVE_INT("ui_sounds", s_cfg.ui_sounds);
    SAVE_INT("theme_mode", s_cfg.theme_mode);
    SAVE_INT("ring_speed", s_cfg.ring_speed);
    SAVE_INT("transitions", s_cfg.transitions);
    SAVE_INT("default_view", s_cfg.default_view);
    SAVE_INT("sort_order", s_cfg.sort_order);
    SAVE_INT("show_subtitle", s_cfg.show_subtitle);
    SAVE_INT("show_battery_pct", s_cfg.show_battery_pct);
    SAVE_INT("clock_12h", s_cfg.clock_12h);
    SAVE_INT("wallpaper_dim", s_cfg.wallpaper_dim);
    SAVE_INT("accent_preset", s_cfg.accent_preset);
    SAVE_INT("custom_r", s_cfg.custom_r);
    SAVE_INT("custom_g", s_cfg.custom_g);
    SAVE_INT("custom_b", s_cfg.custom_b);
    SAVE_INT("sfx_volume", s_cfg.sfx_volume);
    SAVE_INT("bgm_volume", s_cfg.bgm_volume);
    SAVE_STR("wallpaper", s_cfg.wallpaper);
    SAVE_STR("sfx_pack", s_cfg.sfx_pack);
    SAVE_STR("bgm_track", s_cfg.bgm_track);

    #undef SAVE_INT
    #undef SAVE_STR

    char path[PATH_MAX + 64];
    getSettingsPath(path, sizeof(path));
    config_write_file(&cfg, path);
    config_destroy(&cfg);
}

static void switch2SettingsLoad(void) {
    s_cfg = kDefaultCfg;
    config_t cfg;
    config_init(&cfg);
    char path[PATH_MAX + 64];
    getSettingsPath(path, sizeof(path));
    if (config_read_file(&cfg, path) == CONFIG_TRUE) {
        int v = 0;
        const char *str = NULL;
        if (config_lookup_int(&cfg, "ui_sounds", &v))        s_cfg.ui_sounds        = v ? 1 : 0;
        if (config_lookup_int(&cfg, "theme_mode", &v))       s_cfg.theme_mode       = (v >= 0 && v <= 2) ? v : 0;
        if (config_lookup_int(&cfg, "ring_speed", &v))       s_cfg.ring_speed       = (v >= 0 && v <= 3) ? v : 1;
        if (config_lookup_int(&cfg, "transitions", &v))      s_cfg.transitions      = (v >= 0 && v <= 2) ? v : 0;
        if (config_lookup_int(&cfg, "default_view", &v))     s_cfg.default_view     = v ? 1 : 0;
        if (config_lookup_int(&cfg, "sort_order", &v))       s_cfg.sort_order       = v ? 1 : 0;
        if (config_lookup_int(&cfg, "show_subtitle", &v))    s_cfg.show_subtitle    = v ? 1 : 0;
        if (config_lookup_int(&cfg, "show_battery_pct", &v)) s_cfg.show_battery_pct = v ? 1 : 0;
        if (config_lookup_int(&cfg, "clock_12h", &v))        s_cfg.clock_12h        = v ? 1 : 0;
        if (config_lookup_int(&cfg, "wallpaper_dim", &v))    s_cfg.wallpaper_dim    = (v >= 0 && v <= 80) ? v : 30;
        if (config_lookup_int(&cfg, "accent_preset", &v))    s_cfg.accent_preset    = (v >= 0 && v < ACCENT_COUNT) ? v : 0;
        if (config_lookup_int(&cfg, "custom_r", &v))         s_cfg.custom_r         = (v >= 0 && v <= 255) ? v : 78;
        if (config_lookup_int(&cfg, "custom_g", &v))         s_cfg.custom_g         = (v >= 0 && v <= 255) ? v : 224;
        if (config_lookup_int(&cfg, "custom_b", &v))         s_cfg.custom_b         = (v >= 0 && v <= 255) ? v : 245;
        if (config_lookup_int(&cfg, "sfx_volume", &v))       s_cfg.sfx_volume       = (v >= 0 && v <= 100) ? v : 100;
        if (config_lookup_int(&cfg, "bgm_volume", &v))       s_cfg.bgm_volume       = (v >= 0 && v <= 100) ? v : 50;
        if (config_lookup_string(&cfg, "wallpaper", &str) && str) {
            strncpy(s_cfg.wallpaper, str, MEDIA_NAME_MAX - 1);
            s_cfg.wallpaper[MEDIA_NAME_MAX - 1] = '\0';
        }
        if (config_lookup_string(&cfg, "sfx_pack", &str) && str) {
            strncpy(s_cfg.sfx_pack, str, MEDIA_NAME_MAX - 1);
            s_cfg.sfx_pack[MEDIA_NAME_MAX - 1] = '\0';
        }
        if (config_lookup_string(&cfg, "bgm_track", &str) && str) {
            strncpy(s_cfg.bgm_track, str, MEDIA_NAME_MAX - 1);
            s_cfg.bgm_track[MEDIA_NAME_MAX - 1] = '\0';
        }
    }
    config_destroy(&cfg);
}

void switch2UiInit(ThemePreset system_preset) {
    srand((unsigned)time(NULL));
    s_system_preset = system_preset;
    ensureMediaDirs();
    rescanMediaLists();
    switch2SettingsLoad();

    if (s_cfg.theme_mode != 0)
        switch2ApplyThemeSetting();
    switch2ApplyWallpaper();
    switch2ApplySfxPack();
    switch2ApplyBgm();

    s_view = (s_cfg.default_view == 1) ? S2View_Grid : S2View_Carousel;
    s_last_view = s_view;
    s_trans_t = 1.0f;
}

void switch2UiExit(void) {
    switch2WallpaperUnload();
}

/* --- Visible Entry List & Scroll Helpers ---------------------------------- */

static bool hasEndAllSoftwareTile(void) {
    return s_view == S2View_Carousel && !menuIsThemeMenuActive() && s_visible_count > 0;
}

static float carouselMaxScroll(void) {
    int total_slots = s_visible_count + (hasEndAllSoftwareTile() ? 1 : 0);
    float content_w = CAROUSEL_START_X * 2.0f + (float)total_slots * CAROUSEL_STRIDE - (CAROUSEL_STRIDE - CAROUSEL_TILE_SZ);
    float max_scroll = content_w - SCREEN_W;
    return max_scroll > 0.0f ? max_scroll : 0.0f;
}

static float gridMaxScrollY(void) {
    int total_rows = (s_visible_count + GRID_COLS - 1) / GRID_COLS;
    return (total_rows > 2) ? (float)(total_rows - 2) * GRID_STRIDE_Y : 0.0f;
}

typedef struct {
    menuEntry_s *me;
    int orig_idx;
} VisiblePair;

static int cmpVisibleAlpha(const void *a, const void *b) {
    const VisiblePair *pa = (const VisiblePair *)a;
    const VisiblePair *pb = (const VisiblePair *)b;
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

/* Unified entry activation (handles Audit Fix #1 + Switch 2 launch animation + BGM fade-out) */
static void activateEntry(menu_s *menu, int vis_idx) {
    if (!menu || vis_idx < 0 || vis_idx >= s_visible_count)
        return;
    menuEntry_s *me = s_visible[vis_idx];
    menu->curEntry = s_visible_indices[vis_idx];

    if (me->type == ENTRY_TYPE_FOLDER) {
        /* Audit Fix #1: reset Favorites filter & scroll when entering a subfolder */
        switch2SfxPlay(S2Sfx_Confirm);
        s_favorites_only = false;
        s_scroll_x = 0.0f;
        s_grid_scroll_y = 0.0f;
        s_sel = 0;
        launchMenuEntryTask(me);
        syncSelectionFromMenu(menuGetCurrent());
        triggerTransition();
    } else if (me->type == ENTRY_TYPE_THEME) {
        switch2SfxPlay(S2Sfx_Confirm);
        launchMenuEntryTask(me);
        syncSelectionFromMenu(menuGetCurrent());
    } else {
        /* Switch 2 launch animation (~200ms) + smooth BGM fade-out */
        switch2SfxPlay(S2Sfx_Launch);
        switch2BgmStartFadeOut(0.20f);
        s_launching_entry = me;
        s_launch_t = 0.0f;
    }
}

static float pillButtonCenterX(int index) {
    float start_x = SCREEN_W * 0.5f - (PILL_COUNT - 1) * 0.5f * PILL_BTN_STRIDE;
    return start_x + index * PILL_BTN_STRIDE;
}

static void openSettingsView(void) {
    /* Audit Fix #6: exit Theme Menu cleanly if open before entering Settings */
    if (menuIsThemeMenuActive()) {
        launchMenuBackTask();
        s_sel = 0;
        s_scroll_x = 0.0f;
    }
    rescanMediaLists();
    s_prev_view = (s_view == S2View_Grid) ? S2View_Grid : S2View_Carousel;
    s_view = S2View_Settings;
    s_settings_sel = 0;
    s_settings_scroll = 0;
    s_reset_armed = false;
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
        openSettingsView();
        break;
    case PILL_EXIT:
        switch2SfxPlay(S2Sfx_Back);
        s_exit_requested = true;
        break;
    }
}

/* --- Drawing Helpers ------------------------------------------------------ */

static void drawIconAlpha(int x, int y, int width, int height, const uint8_t *image, color_t color) {
    renderImage(image, x, y, width, height, RenderImageFormat_AlphaMask, color.abgr | 0xff000000u);
}

static void drawHorizontalBattery(float x, float y, uint32_t charge, bool charging, color_t fg, bool dark) {
    uint32_t col = fg.abgr | 0xff000000u;
    uint32_t bg = dark ? makeRGBA(24, 24, 26, 255) : makeRGBA(235, 235, 238, 255);
    renderRoundedRectF(x, y, 36.0f, 19.0f, 4.0f, col);
    renderRoundedRectF(x + 2.2f, y + 2.2f, 31.6f, 14.6f, 2.2f, bg);
    renderRoundedRectF(x + 36.5f, y + 5.5f, 3.0f, 8.0f, 1.5f, col);
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

    /* Left: Switch 2 circular badge */
    float cx = 74.0f, cy = bar_cy, br = 24.0f;
    uint32_t badge_bg = dark ? makeRGBA(46, 48, 56, 255) : makeRGBA(255, 255, 255, 255);
    uint32_t badge_rim = dark ? makeRGBA(190, 194, 204, 255) : makeRGBA(185, 190, 200, 255);
    renderSoftShadowF(cx - br, cy - br + 2.0f, br * 2.0f, br * 2.0f, br, 6.0f, dark ? 55 : 26);
    renderCircleF(cx, cy, br + 2.0f, badge_rim);
    renderCircleF(cx, cy, br, badge_bg);
    renderRoundedRectF(cx - 11.0f, cy - 10.5f, 9.5f, 21.0f, 4.75f, makeRGBA(0, 195, 227, 255));
    renderRoundedRectF(cx + 1.5f, cy - 10.5f, 9.5f, 21.0f, 4.75f, makeRGBA(255, 69, 84, 255));

    const char *title = "Homebrew Menu";
    uint32_t tw = 0, th = 0;
    GetTextDimensions(interuiregular18, title, &tw, &th);
    DrawText(interuiregular18, 114, (uint32_t)(bar_cy + fontBaselineOffset(interuiregular18)), makeColorMuted(txt, dark ? 210 : 200), title);

    float tag_x = 114.0f + (float)tw + 14.0f;

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

    /* Right status: [Applet Mode]  HH:MM  [Wi-Fi]  [Pct%] [Battery] */
    float right_x = 1232.0f;

    uint32_t batteryCharge = 100;
    bool isCharging = false;
    if (powerGetDetails(&batteryCharge, &isCharging)) {
        if (batteryCharge > 100) batteryCharge = 100;
        right_x -= 40.0f;
        drawHorizontalBattery(right_x, bar_cy - 9.5f, batteryCharge, isCharging, txt, dark);
        if (s_cfg.show_battery_pct) {
            char pctStr[16];
            snprintf(pctStr, sizeof(pctStr), "%u%%", (unsigned)batteryCharge);
            uint32_t bw = 0, bh = 0;
            GetTextDimensions(interuiregular18, pctStr, &bw, &bh);
            right_x -= ((float)bw + 10.0f);
            DrawText(interuiregular18, (uint32_t)right_x, (uint32_t)(bar_cy + fontBaselineOffset(interuiregular18)), txt, pctStr);
            right_x -= 18.0f;
        } else {
            right_x -= 22.0f;
        }
    }

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

    char timeStr[24];
    formatClockString(timeStr, sizeof(timeStr));
    uint32_t cw = 0, ch = 0;
    GetTextDimensions(interuimedium20, timeStr, &cw, &ch);
    right_x -= (float)cw;
    DrawText(interuimedium20, (uint32_t)right_x, (uint32_t)(bar_cy + fontBaselineOffset(interuimedium20)), txt, timeStr);
    right_x -= 18.0f;

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
    float launch_boost = (s_launching_entry && s_launching_entry == me) ? (s_launch_t * 0.12f) : 0.0f;
    float scale = (is_focused ? (1.0f + 0.026f * pop_amount) : 1.0f) + launch_boost;
    float draw_sz = size * scale;
    float dx = x - (draw_sz - size) * 0.5f;
    float dy = y - (draw_sz - size) * 0.5f;
    float radius = draw_sz * 0.072f;

    renderSoftShadowF(dx, dy + (is_focused ? 7.0f : 3.0f), draw_sz, draw_sz,
                      radius, is_focused ? 14.0f : 7.0f,
                      dark ? (is_focused ? 120 : 55) : (is_focused ? 65 : 30));

    uint32_t card_bg = dark ? makeRGBA(38, 40, 46, 255) : makeRGBA(255, 255, 255, 255);
    renderRoundedRectF(dx, dy, draw_sz, draw_sz, radius, card_bg);

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

    if (me->starred && me->type != ENTRY_TYPE_THEME) {
        float bx = dx + draw_sz - 22.0f;
        float by = dy + 22.0f;
        renderStarF(bx, by + 1.5f, 11.5f, makeRGBA(0, 0, 0, 125));
        renderStarF(bx, by, 10.5f, makeRGBA(255, 206, 56, 255));
    }

    if (is_focused) {
        float pad = 6.5f;
        uint32_t rc1 = 0, rc2 = 0;
        getFocusRingColors(&rc1, &rc2);
        renderSwitch2FocusRingF(dx - pad, dy - pad, draw_sz + pad * 2.0f, draw_sz + pad * 2.0f,
                                radius + pad, 4.0f, ringTimer(), rc1, rc2);
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

    float sq = r * 0.34f;
    float gap = r * 0.12f;
    float off = sq + gap * 0.5f;
    renderRoundedRectF(cx - off, cy - off, sq, sq, sq * 0.25f, fg);
    renderRoundedRectF(cx + gap * 0.5f, cy - off, sq, sq, sq * 0.25f, fg);
    renderRoundedRectF(cx - off, cy + gap * 0.5f, sq, sq, sq * 0.25f, fg);
    renderRoundedRectF(cx + gap * 0.5f, cy + gap * 0.5f, sq, sq, sq * 0.25f, fg);

    if (is_focused) {
        uint32_t rc1 = 0, rc2 = 0;
        getFocusRingColors(&rc1, &rc2);
        renderSwitch2CircleFocusRingF(cx, cy, r + 6.0f, 5.0f, ringTimer(), rc1, rc2);
    }
}

static void drawFocusedHeaderInfo(menuEntry_s *me, float center_x, float title_y, bool is_all_software_tile) {
    bool dark = isDarkTheme();
    color_t title_col = getAccentColor(dark);
    color_t muted = makeColorMuted(themeCurrent.textColor, dark ? 165 : 155);

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

    float max_scroll = carouselMaxScroll();

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

    float slide_y = transSlideOffset();
    float tile_y = CAROUSEL_TILE_Y + slide_y;
    for (int i = 0; i < total_slots; i++) {
        float tx = CAROUSEL_START_X + (float)i * CAROUSEL_STRIDE - s_scroll_x;
        if (tx + CAROUSEL_TILE_SZ < -40.0f || tx > SCREEN_W + 40.0f)
            continue;
        bool is_active = (s_focus == S2Focus_Apps && i == s_sel);
        if (is_active)
            continue;
        if (i < s_visible_count)
            drawAppTile(s_visible[i], tx, tile_y, CAROUSEL_TILE_SZ, false, 0.0f);
        else
            drawAllSoftwareEndTile(tx, tile_y, CAROUSEL_TILE_SZ, false, 0.0f);
    }

    if (s_focus == S2Focus_Apps && s_sel >= 0 && s_sel < total_slots) {
        float focused_x = CAROUSEL_START_X + (float)s_sel * CAROUSEL_STRIDE - s_scroll_x;
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
    float max_scroll_y = gridMaxScrollY();
    if (!s_touch_active) {
        float target_y = (sel_row > 0) ? (float)(sel_row - 1) * GRID_STRIDE_Y : 0.0f;
        if (target_y > max_scroll_y) target_y = max_scroll_y;
        s_grid_scroll_y += (target_y - s_grid_scroll_y) * 0.22f;
    }

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

    if (s_focus == S2Focus_Apps && s_sel >= 0 && s_sel < s_visible_count) {
        int col = s_sel % GRID_COLS;
        int row = s_sel / GRID_COLS;
        float tx = GRID_START_X + (float)col * GRID_STRIDE_X;
        float ty = GRID_START_Y + (float)row * GRID_STRIDE_Y - s_grid_scroll_y + slide_y;
        if (ty + GRID_TILE_SZ >= GRID_CLIP_TOP - 20.0f && ty <= GRID_CLIP_BOTTOM + 20.0f)
            drawAppTile(s_visible[s_sel], tx, ty, GRID_TILE_SZ, true, s_app_pop);
    }

    /* Top & bottom occlusion bands (semi-transparent when wallpaper is active) */
    uint8_t band_a = switch2WallpaperIsLoaded() ? 205 : 255;
    uint32_t bg_band = (themeCurrent.backgroundColor.abgr & 0x00ffffffu) | ((uint32_t)band_a << 24);
    renderRectF(0.0f, 0.0f, SCREEN_W, GRID_CLIP_TOP, bg_band);
    renderRectF(0.0f, GRID_CLIP_BOTTOM, SCREEN_W, SCREEN_H - GRID_CLIP_BOTTOM, bg_band);

    if (s_focus == S2Focus_Apps && s_sel < s_visible_count) {
        drawFocusedHeaderInfo(s_visible[s_sel], SCREEN_W * 0.5f, 112.0f, false);
    }
}

static void drawPillIconSymbol(int idx, float cx, float cy, float scale, uint32_t fg, uint32_t hole_bg) {
    float s = scale * 1.15f;
    switch (idx) {
    case PILL_NETLOADER:
        renderNetloaderIconF(cx, cy, 17.5f * scale, fg);
        break;
    case PILL_FAVORITES:
        renderStarF(cx, cy, 14.5f * scale, fg);
        break;
    case PILL_ALL_SOFTWARE: {
        float sq = 9.0f * s;
        float gap = 3.6f * s;
        renderRoundedRectF(cx - sq - gap * 0.5f, cy - sq - gap * 0.5f, sq, sq, 2.4f * s, fg);
        renderRoundedRectF(cx + gap * 0.5f, cy - sq - gap * 0.5f, sq, sq, 2.4f * s, fg);
        renderRoundedRectF(cx - sq - gap * 0.5f, cy + gap * 0.5f, sq, sq, 2.4f * s, fg);
        renderRoundedRectF(cx + gap * 0.5f, cy + gap * 0.5f, sq, sq, 2.4f * s, fg);
        break;
    }
    case PILL_THEMES:
        renderRoundedRectF(cx - 11.5f * s, cy - 9.0f * s, 16.0f * s, 12.5f * s, 3.2f * s, fg);
        renderRoundedRectF(cx - 4.0f * s, cy - 2.0f * s, 16.0f * s, 12.5f * s, 3.2f * s,
                           (fg & 0x00ffffffu) | 0xc8000000u);
        break;
    case PILL_SETTINGS:
        renderSettingsIconF(cx, cy, 16.5f * scale, fg);
        break;
    case PILL_EXIT:
        renderCircleF(cx, cy + 1.5f * s, 11.5f * s, fg);
        renderCircleF(cx, cy + 1.5f * s, 8.2f * s, hole_bg);
        renderRectF(cx - 4.5f * s, cy - 11.5f * s, 9.0f * s, 10.0f * s, hole_bg);
        renderRoundedRectF(cx - 1.8f * s, cy - 12.5f * s, 3.6f * s, 12.0f * s, 1.8f * s, fg);
        break;
    }
}

static void drawBottomPillBar(void) {
    bool dark = isDarkTheme();
    color_t accent = getAccentColor(dark);

    uint32_t pill_bg = dark ? makeRGBA(45, 45, 49, 245) : makeRGBA(255, 255, 255, 248);

    renderSoftShadowF(PILL_X, PILL_Y + 5.0f, PILL_W, PILL_H, PILL_H * 0.5f, 16.0f, dark ? 105 : 38);
    renderRoundedRectF(PILL_X, PILL_Y, PILL_W, PILL_H, PILL_H * 0.5f, pill_bg);

    uint32_t icon_colors[PILL_COUNT] = {
        makeRGBA(42, 206, 232, 255),
        makeRGBA(248, 178, 38, 255),
        makeRGBA(238, 66, 74, 255),
        makeRGBA(146, 114, 255, 255),
        dark ? makeRGBA(212, 214, 222, 255) : makeRGBA(82, 86, 96, 255),
        dark ? makeRGBA(212, 214, 222, 255) : makeRGBA(82, 86, 96, 255),
    };
    static const char *kPillLabels[PILL_COUNT] = {
        "Netloader", "Favorites", "All Software", "Themes", "Settings", "Exit",
    };

    uint32_t rc1 = 0, rc2 = 0;
    getFocusRingColors(&rc1, &rc2);

    for (int i = 0; i < PILL_COUNT; i++) {
        float cx = pillButtonCenterX(i);
        float cy = PILL_CENTER_Y;
        bool focused = (s_focus == S2Focus_Pill && s_pill_idx == i);
        float scale = focused ? (1.0f + 0.10f * s_pill_pop) : 1.0f;
        float r = (PILL_BTN_RADIUS + 4.0f) * scale;

        if (focused) {
            uint32_t foc_bg = dark ? makeRGBA(58, 60, 68, 255) : makeRGBA(240, 244, 250, 255);
            renderCircleF(cx, cy, r, foc_bg);
            renderSwitch2CircleFocusRingF(cx, cy, r + 3.5f, 4.0f, ringTimer(), rc1, rc2);
        }

        uint32_t hole_bg = focused
            ? (dark ? makeRGBA(58, 60, 68, 255) : makeRGBA(240, 244, 250, 255))
            : pill_bg;
        drawPillIconSymbol(i, cx, cy, scale, icon_colors[i], hole_bg);

        bool is_toggled = (i == PILL_FAVORITES && s_favorites_only) ||
                          (i == PILL_ALL_SOFTWARE && s_view == S2View_Grid) ||
                          (i == PILL_THEMES && menuIsThemeMenuActive());
        if (is_toggled) {
            renderCircleF(cx, PILL_Y + PILL_H - 7.0f, 3.5f, accent.abgr | 0xff000000u);
        }

        if (focused) {
            const char *lbl = kPillLabels[i];
            uint32_t lw = 0, lh = 0;
            GetTextDimensions(interuiregular18, lbl, &lw, &lh);
            float lx = cx - (float)lw * 0.5f;
            float ly = PILL_Y + PILL_H + 28.0f;
            DrawText(interuiregular18, (uint32_t)lx, (uint32_t)ly, accent, lbl);
        }
    }
}

static void drawBottomCommandStrip(menu_s *menu) {
    bool dark = isDarkTheme();
    color_t txt = themeCurrent.textColor;
    color_t muted = makeColorMuted(txt, dark ? 165 : 155);

    float jx = 50.0f, jy = 660.0f;
    uint32_t jc_col = dark ? makeRGBA(225, 227, 232, 255) : makeRGBA(62, 65, 74, 255);
    uint32_t led_on = makeRGBA(130, 235, 65, 255);
    uint32_t led_off = dark ? makeRGBA(76, 78, 86, 255) : makeRGBA(185, 188, 196, 255);
    renderRectF(jx, jy + 2.0f, 4.0f, 4.0f, led_on);
    renderRectF(jx, jy + 9.0f, 4.0f, 4.0f, led_off);
    renderRectF(jx, jy + 16.0f, 4.0f, 4.0f, led_off);
    renderRectF(jx, jy + 23.0f, 4.0f, 4.0f, led_off);
    renderRoundedRectF(jx + 12.0f, jy, 12.0f, 30.0f, 5.5f, jc_col);
    renderRoundedRectF(jx + 27.0f, jy, 12.0f, 30.0f, 5.5f, jc_col);
    uint32_t bg_cut = themeCurrent.backgroundColor.abgr | 0xff000000u;
    renderCircleF(jx + 18.0f, jy + 9.0f, 2.5f, bg_cut);
    renderCircleF(jx + 33.0f, jy + 19.0f, 2.5f, bg_cut);

    char infoBuf[PATH_MAX + 64];
    if (menuIsThemeMenuActive()) {
        snprintf(infoBuf, sizeof(infoBuf), "Themes (%d)", s_visible_count);
    } else if (s_favorites_only) {
        snprintf(infoBuf, sizeof(infoBuf), "Favorites (%d)", s_visible_count);
    } else {
        snprintf(infoBuf, sizeof(infoBuf), "%d apps", s_visible_count);
    }
    DrawTextTruncate(interuiregular14, 104, 681, muted, infoBuf, 400, "...");

    float rx = 1232.0f;
    const uint32_t y_txt = 682;

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

/* --- Sidebar Settings Model & UI ------------------------------------------ */

static int getCategoryItems(S2SettingsCat cat, int *out_ids) {
    int n = 0;
    if (cat == S2Cat_General) {
        out_ids[n++] = SET_DEFAULT_VIEW;
        out_ids[n++] = SET_SORT_ORDER;
        out_ids[n++] = SET_SHOW_SUBTITLE;
        out_ids[n++] = SET_BATTERY_PCT;
        out_ids[n++] = SET_CLOCK_FORMAT;
        out_ids[n++] = SET_TRANSITIONS;
        out_ids[n++] = SET_RESET_DEFAULTS;
    } else if (cat == S2Cat_Appearance) {
        out_ids[n++] = SET_THEME;
        out_ids[n++] = SET_WALLPAPER;
        out_ids[n++] = SET_WALLPAPER_DIM;
        out_ids[n++] = SET_RING_SPEED;
        out_ids[n++] = SET_ACCENT_PRESET;
        if (s_cfg.accent_preset == ACCENT_CUSTOM_RGB) {
            out_ids[n++] = SET_CUSTOM_R;
            out_ids[n++] = SET_CUSTOM_G;
            out_ids[n++] = SET_CUSTOM_B;
        }
    } else if (cat == S2Cat_Audio) {
        out_ids[n++] = SET_SOUNDS;
        out_ids[n++] = SET_SFX_PACK;
        out_ids[n++] = SET_SFX_VOL;
        out_ids[n++] = SET_BGM_TRACK;
        out_ids[n++] = SET_BGM_VOL;
    }
    return n;
}

static const char *settingLabel(int id) {
    switch (id) {
    case SET_DEFAULT_VIEW:   return "Default View";
    case SET_SORT_ORDER:     return "Sort Order";
    case SET_SHOW_SUBTITLE:  return "Show Author & Version";
    case SET_BATTERY_PCT:    return "Battery Percentage";
    case SET_CLOCK_FORMAT:   return "Clock Format";
    case SET_TRANSITIONS:    return "Transitions";
    case SET_RESET_DEFAULTS: return "Reset to Defaults";
    case SET_THEME:          return "Theme";
    case SET_WALLPAPER:      return "Wallpaper";
    case SET_WALLPAPER_DIM:  return "Wallpaper Dimming";
    case SET_RING_SPEED:     return "Focus Border Speed";
    case SET_ACCENT_PRESET:  return "Focus & Accent Color";
    case SET_CUSTOM_R:       return "Custom Red";
    case SET_CUSTOM_G:       return "Custom Green";
    case SET_CUSTOM_B:       return "Custom Blue";
    case SET_SOUNDS:         return "UI Sounds";
    case SET_SFX_PACK:       return "Sound Pack (.zip)";
    case SET_SFX_VOL:        return "SFX Volume";
    case SET_BGM_TRACK:      return "Background Music";
    case SET_BGM_VOL:        return "BGM Volume";
    default:                 return "";
    }
}

static void formatSettingValue(int id, char *buf, size_t sz) {
    switch (id) {
    case SET_DEFAULT_VIEW:
        snprintf(buf, sz, "%s", s_cfg.default_view ? "All Software" : "Carousel");
        break;
    case SET_SORT_ORDER:
        snprintf(buf, sz, "%s", s_cfg.sort_order ? "Alphabetical" : "Favorites First");
        break;
    case SET_SHOW_SUBTITLE:
        snprintf(buf, sz, "%s", s_cfg.show_subtitle ? "On" : "Off");
        break;
    case SET_BATTERY_PCT:
        snprintf(buf, sz, "%s", s_cfg.show_battery_pct ? "On" : "Off");
        break;
    case SET_CLOCK_FORMAT:
        snprintf(buf, sz, "%s", s_cfg.clock_12h ? "12-Hour" : "24-Hour");
        break;
    case SET_TRANSITIONS:
        snprintf(buf, sz, "%s", s_cfg.transitions == 0 ? "Slide & Fade" : (s_cfg.transitions == 1 ? "Fade" : "Instant"));
        break;
    case SET_RESET_DEFAULTS:
        snprintf(buf, sz, "%s", s_reset_armed ? "Press A again to confirm" : "Reset");
        break;
    case SET_THEME:
        snprintf(buf, sz, "%s", s_cfg.theme_mode == 1 ? "Basic Light" : (s_cfg.theme_mode == 2 ? "Basic Dark" : "Auto (System)"));
        break;
    case SET_WALLPAPER:
        snprintf(buf, sz, "%s", s_cfg.wallpaper[0] ? s_cfg.wallpaper : "None");
        break;
    case SET_WALLPAPER_DIM:
        snprintf(buf, sz, "%d%%", s_cfg.wallpaper_dim);
        break;
    case SET_RING_SPEED:
        snprintf(buf, sz, "%s", s_cfg.ring_speed == 0 ? "Slow" : (s_cfg.ring_speed == 1 ? "Normal" : (s_cfg.ring_speed == 2 ? "Fast" : "Static")));
        break;
    case SET_ACCENT_PRESET: {
        static const char *kNames[ACCENT_COUNT] = {
            "Iridescent", "Neon Cyan", "Switch 2 Red", "Electric Violet", "Gold", "Emerald", "Custom RGB"
        };
        int idx = (s_cfg.accent_preset >= 0 && s_cfg.accent_preset < ACCENT_COUNT) ? s_cfg.accent_preset : 0;
        snprintf(buf, sz, "%s", kNames[idx]);
        break;
    }
    case SET_CUSTOM_R:
        snprintf(buf, sz, "%d", s_cfg.custom_r);
        break;
    case SET_CUSTOM_G:
        snprintf(buf, sz, "%d", s_cfg.custom_g);
        break;
    case SET_CUSTOM_B:
        snprintf(buf, sz, "%d", s_cfg.custom_b);
        break;
    case SET_SOUNDS:
        snprintf(buf, sz, "%s", s_cfg.ui_sounds ? "On" : "Off");
        break;
    case SET_SFX_PACK:
        snprintf(buf, sz, "%s", s_cfg.sfx_pack[0] ? s_cfg.sfx_pack : "Built-in (Switch 2)");
        break;
    case SET_SFX_VOL:
        snprintf(buf, sz, "%d%%", s_cfg.sfx_volume);
        break;
    case SET_BGM_TRACK:
        if (!s_cfg.bgm_track[0]) snprintf(buf, sz, "Off");
        else if (strcmp(s_cfg.bgm_track, "__PACK__") == 0) snprintf(buf, sz, "Sound Pack BGM");
        else if (strcmp(s_cfg.bgm_track, "__RANDOM__") == 0) snprintf(buf, sz, "Random");
        else snprintf(buf, sz, "%s", s_cfg.bgm_track);
        break;
    case SET_BGM_VOL:
        snprintf(buf, sz, "%d%%", s_cfg.bgm_volume);
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void cycleMediaSelection(char *current, char list[MAX_MEDIA_FILES][MEDIA_NAME_MAX], int count, int delta) {
    int cur = -1;
    if (current[0]) {
        for (int i = 0; i < count; i++) {
            if (strcmp(current, list[i]) == 0) { cur = i; break; }
        }
    }
    int total = count + 1; /* 0 = None/Built-in, 1..count = list[i-1] */
    int idx = (cur + 1 + delta + total) % total;
    if (idx == 0) current[0] = '\0';
    else {
        strncpy(current, list[idx - 1], MEDIA_NAME_MAX - 1);
        current[MEDIA_NAME_MAX - 1] = '\0';
    }
}

static void cycleBgmSelection(int delta) {
    /* Options: 0=Off, [optional Pack BGM], [optional Random], tracks... */
    char opts[MAX_MEDIA_FILES + 3][MEDIA_NAME_MAX];
    memset(opts, 0, sizeof(opts));
    int n = 0;
    n++; /* index 0 = "" (Off) */
    if (s_cfg.sfx_pack[0] && switch2SfxPackHasBgm()) {
        snprintf(opts[n++], MEDIA_NAME_MAX, "__PACK__");
    }
    if (s_bgm_track_count > 0) {
        snprintf(opts[n++], MEDIA_NAME_MAX, "__RANDOM__");
    }
    for (int i = 0; i < s_bgm_track_count; i++) {
        snprintf(opts[n++], MEDIA_NAME_MAX, "%.127s", s_bgm_tracks[i]);
    }

    int cur = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(s_cfg.bgm_track, opts[i]) == 0) { cur = i; break; }
    }
    int next = (cur + delta + n) % n;
    strncpy(s_cfg.bgm_track, opts[next], MEDIA_NAME_MAX - 1);
    s_cfg.bgm_track[MEDIA_NAME_MAX - 1] = '\0';
}

static void changeSettingById(int id, int delta, bool is_a_press) {
    if (id != SET_RESET_DEFAULTS)
        s_reset_armed = false;

    switch (id) {
    case SET_DEFAULT_VIEW:
        s_cfg.default_view = s_cfg.default_view ? 0 : 1;
        break;
    case SET_SORT_ORDER:
        s_cfg.sort_order = s_cfg.sort_order ? 0 : 1;
        break;
    case SET_SHOW_SUBTITLE:
        s_cfg.show_subtitle = s_cfg.show_subtitle ? 0 : 1;
        break;
    case SET_BATTERY_PCT:
        s_cfg.show_battery_pct = s_cfg.show_battery_pct ? 0 : 1;
        break;
    case SET_CLOCK_FORMAT:
        s_cfg.clock_12h = s_cfg.clock_12h ? 0 : 1;
        break;
    case SET_TRANSITIONS:
        s_cfg.transitions = (s_cfg.transitions + delta + 3) % 3;
        break;
    case SET_RESET_DEFAULTS:
        if (!is_a_press) return;
        if (!s_reset_armed) {
            s_reset_armed = true;
            switch2SfxPlay(S2Sfx_Pill);
            return;
        }
        s_reset_armed = false;
        s_cfg = kDefaultCfg;
        switch2ApplyThemeSetting();
        switch2ApplyWallpaper();
        switch2ApplySfxPack();
        switch2ApplyBgm();
        switch2SettingsSave();
        switch2SfxPlay(S2Sfx_Confirm);
        return;
    case SET_THEME:
        s_cfg.theme_mode = (s_cfg.theme_mode + delta + 3) % 3;
        switch2ApplyThemeSetting();
        break;
    case SET_WALLPAPER:
        cycleMediaSelection(s_cfg.wallpaper, s_wallpapers, s_wallpaper_count, delta);
        switch2ApplyWallpaper();
        break;
    case SET_WALLPAPER_DIM:
        s_cfg.wallpaper_dim += delta * 10;
        if (s_cfg.wallpaper_dim < 0) s_cfg.wallpaper_dim = 0;
        if (s_cfg.wallpaper_dim > 80) s_cfg.wallpaper_dim = 80;
        break;
    case SET_RING_SPEED:
        s_cfg.ring_speed = (s_cfg.ring_speed + delta + 4) % 4;
        break;
    case SET_ACCENT_PRESET:
        s_cfg.accent_preset = (s_cfg.accent_preset + delta + ACCENT_COUNT) % ACCENT_COUNT;
        break;
    case SET_CUSTOM_R:
        s_cfg.custom_r += delta * 15;
        if (s_cfg.custom_r < 0) s_cfg.custom_r = 0;
        if (s_cfg.custom_r > 255) s_cfg.custom_r = 255;
        break;
    case SET_CUSTOM_G:
        s_cfg.custom_g += delta * 15;
        if (s_cfg.custom_g < 0) s_cfg.custom_g = 0;
        if (s_cfg.custom_g > 255) s_cfg.custom_g = 255;
        break;
    case SET_CUSTOM_B:
        s_cfg.custom_b += delta * 15;
        if (s_cfg.custom_b < 0) s_cfg.custom_b = 0;
        if (s_cfg.custom_b > 255) s_cfg.custom_b = 255;
        break;
    case SET_SOUNDS:
        s_cfg.ui_sounds = s_cfg.ui_sounds ? 0 : 1;
        switch2SfxSetEnabled(s_cfg.ui_sounds != 0);
        break;
    case SET_SFX_PACK:
        cycleMediaSelection(s_cfg.sfx_pack, s_sfx_packs, s_sfx_pack_count, delta);
        switch2ApplySfxPack();
        break;
    case SET_SFX_VOL:
        s_cfg.sfx_volume += delta * 10;
        if (s_cfg.sfx_volume < 0) s_cfg.sfx_volume = 0;
        if (s_cfg.sfx_volume > 100) s_cfg.sfx_volume = 100;
        switch2SfxSetVolume(s_cfg.sfx_volume);
        break;
    case SET_BGM_TRACK:
        cycleBgmSelection(delta);
        switch2ApplyBgm();
        break;
    case SET_BGM_VOL:
        s_cfg.bgm_volume += delta * 10;
        if (s_cfg.bgm_volume < 0) s_cfg.bgm_volume = 0;
        if (s_cfg.bgm_volume > 100) s_cfg.bgm_volume = 100;
        switch2BgmSetVolume(s_cfg.bgm_volume);
        break;
    }

    switch2SettingsSave();
    switch2SfxPlay(S2Sfx_Confirm);
}

#define SETTINGS_SIDEBAR_X  56.0f
#define SETTINGS_SIDEBAR_W  260.0f
#define SETTINGS_CAT_Y0     122.0f
#define SETTINGS_CAT_H      56.0f
#define SETTINGS_CAT_GAP    8.0f

#define SETTINGS_LIST_X     388.0f
#define SETTINGS_LIST_Y     116.0f
#define SETTINGS_LIST_W     828.0f
#define SETTINGS_ROW_H      64.0f
#define SETTINGS_MAX_ROWS   8

static float transEase(void) {
    float t = s_trans_t;
    if (t >= 1.0f) return 1.0f;
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
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
    color_t muted = makeColorMuted(txt, dark ? 160 : 150);
    color_t accent = getAccentColor(dark);
    uint32_t sep_col = dark ? makeRGBA(62, 64, 72, 220) : makeRGBA(212, 215, 222, 220);

    /* Subtle frosted backdrop over custom wallpaper so Settings stays super crisp */
    if (switch2WallpaperIsLoaded()) {
        uint32_t veil = (themeCurrent.backgroundColor.abgr & 0x00ffffffu) | ((uint32_t)(dark ? 195 : 210) << 24);
        renderRectF(0.0f, 0.0f, SCREEN_W, SCREEN_H, veil);
    }

    const float bar_cy = 50.0f;
    renderSettingsIconF(74.0f, bar_cy, 18.0f, txt.abgr | 0xff000000u);
    DrawText(interuimedium20, 108, (uint32_t)(bar_cy + fontBaselineOffset(interuimedium20)), txt, "Settings");

    /* Right status: Clock + [Pct%] + Battery */
    float right_x = 1232.0f;
    uint32_t batteryCharge = 100;
    bool isCharging = false;
    if (powerGetDetails(&batteryCharge, &isCharging)) {
        if (batteryCharge > 100) batteryCharge = 100;
        right_x -= 40.0f;
        drawHorizontalBattery(right_x, bar_cy - 9.5f, batteryCharge, isCharging, txt, dark);
        if (s_cfg.show_battery_pct) {
            char pctStr[16];
            snprintf(pctStr, sizeof(pctStr), "%u%%", (unsigned)batteryCharge);
            uint32_t bw = 0, bh = 0;
            GetTextDimensions(interuiregular18, pctStr, &bw, &bh);
            right_x -= ((float)bw + 10.0f);
            DrawText(interuiregular18, (uint32_t)right_x, (uint32_t)(bar_cy + fontBaselineOffset(interuiregular18)), txt, pctStr);
            right_x -= 18.0f;
        } else {
            right_x -= 22.0f;
        }
    }
    char timeStr[24];
    formatClockString(timeStr, sizeof(timeStr));
    uint32_t cw = 0, ch = 0;
    GetTextDimensions(interuimedium20, timeStr, &cw, &ch);
    right_x -= (float)cw;
    DrawText(interuimedium20, (uint32_t)right_x, (uint32_t)(bar_cy + fontBaselineOffset(interuimedium20)), txt, timeStr);

    /* Dividers */
    renderRectF(42.0f, 92.0f, SCREEN_W - 84.0f, 1.0f, sep_col);
    renderRectF(344.0f, 114.0f, 1.0f, 514.0f, sep_col);

    /* Left Sidebar Categories */
    static const char *kCatNames[S2Cat_Count] = { "General", "Appearance", "Audio" };
    for (int c = 0; c < S2Cat_Count; c++) {
        float cy = SETTINGS_CAT_Y0 + (float)c * (SETTINGS_CAT_H + SETTINGS_CAT_GAP);
        bool active = (c == (int)s_settings_cat);
        if (active) {
            uint32_t cat_bg = dark ? makeRGBA(44, 46, 54, 245) : makeRGBA(246, 248, 252, 250);
            renderRoundedRectF(SETTINGS_SIDEBAR_X, cy, SETTINGS_SIDEBAR_W, SETTINGS_CAT_H, 10.0f, cat_bg);
            renderRoundedRectF(SETTINGS_SIDEBAR_X + 10.0f, cy + 12.0f, 4.5f, 32.0f, 2.25f, accent.abgr | 0xff000000u);
        }
        DrawText(interuimedium20, (uint32_t)(SETTINGS_SIDEBAR_X + 30.0f),
                 (uint32_t)(cy + SETTINGS_CAT_H * 0.5f + fontBaselineOffset(interuimedium20)),
                 active ? accent : makeColorMuted(txt, dark ? 200 : 190), kCatNames[c]);
    }

    /* Live Preview Card in lower-left sidebar (when in Appearance) */
    if (s_settings_cat == S2Cat_Appearance) {
        float px = 64.0f, py = 336.0f, pw = 244.0f, ph = 138.0f;
        renderSoftShadowF(px, py + 4.0f, pw, ph, 12.0f, 10.0f, dark ? 95 : 40);
        renderRoundedRectF(px, py, pw, ph, 12.0f, themeCurrent.backgroundColor.abgr | 0xff000000u);
        if (switch2WallpaperIsLoaded()) {
            switch2WallpaperDrawRect(px + 2.0f, py + 2.0f, pw - 4.0f, ph - 4.0f, s_cfg.wallpaper_dim);
        }
        /* Mini focused tile preview with live ring color */
        float tx = px + pw * 0.5f - 28.0f;
        float ty = py + ph * 0.5f - 28.0f;
        renderRoundedRectF(tx, ty, 56.0f, 56.0f, 8.0f, dark ? makeRGBA(52, 54, 64, 255) : makeRGBA(255, 255, 255, 255));
        uint32_t rc1 = 0, rc2 = 0;
        getFocusRingColors(&rc1, &rc2);
        renderSwitch2FocusRingF(tx - 4.0f, ty - 4.0f, 64.0f, 64.0f, 11.0f, 3.0f, ringTimer(), rc1, rc2);
        DrawText(interuiregular14, (uint32_t)px, (uint32_t)(py + ph + 24.0f), muted, "Live Preview");
    } else if (s_settings_cat == S2Cat_Audio) {
        char info1[64], info2[64];
        snprintf(info1, sizeof(info1), "Sound Packs: %d .zip", s_sfx_pack_count);
        snprintf(info2, sizeof(info2), "BGM Tracks: %d", s_bgm_track_count);
        DrawText(interuiregular14, 68, 360, muted, info1);
        DrawText(interuiregular14, 68, 384, muted, info2);
        DrawText(interuiregular14, 68, 418, muted, "Put files in:");
        DrawText(interuiregular14, 68, 440, muted, "config/nx-hbmenu/sfx/");
        DrawText(interuiregular14, 68, 462, muted, "config/nx-hbmenu/bgm/");
    }

    DrawText(interuiregular14, 68, 550, muted, "Switch 2 HOME UI v2.0");
    DrawText(interuiregular14, 68, 572, muted, "by ImDella");
    DrawText(interuiregular14, 68, 594, muted, "Base: ouncebrew / switchbrew");
    DrawText(interuiregular14, 68, 616, muted, "Saved automatically to SD");

    /* Right Settings List */
    int items[16];
    int count = getCategoryItems(s_settings_cat, items);
    if (s_settings_sel >= count) s_settings_sel = count > 0 ? count - 1 : 0;
    if (s_settings_sel < s_settings_scroll) s_settings_scroll = s_settings_sel;
    if (s_settings_sel >= s_settings_scroll + SETTINGS_MAX_ROWS)
        s_settings_scroll = s_settings_sel - SETTINGS_MAX_ROWS + 1;
    if (s_settings_scroll < 0) s_settings_scroll = 0;

    int vis = count - s_settings_scroll;
    if (vis > SETTINGS_MAX_ROWS) vis = SETTINGS_MAX_ROWS;

    float y_off = transSlideOffset();
    for (int row = 0; row < vis; row++) {
        int idx = s_settings_scroll + row;
        int id = items[idx];
        float ry = SETTINGS_LIST_Y + (float)row * SETTINGS_ROW_H + y_off;
        float rcy = ry + SETTINGS_ROW_H * 0.5f;
        bool focused = (idx == s_settings_sel);

        if (row > 0)
            renderRectF(SETTINGS_LIST_X + 12.0f, ry, SETTINGS_LIST_W - 24.0f, 1.0f, sep_col);

        if (focused) {
            uint32_t row_bg = dark ? makeRGBA(44, 46, 54, 245) : makeRGBA(255, 255, 255, 250);
            renderRoundedRectF(SETTINGS_LIST_X, ry + 4.0f, SETTINGS_LIST_W, SETTINGS_ROW_H - 8.0f, 12.0f, row_bg);
            renderRoundedRectF(SETTINGS_LIST_X + 10.0f, ry + 16.0f, 4.5f, SETTINGS_ROW_H - 32.0f, 2.25f, accent.abgr | 0xff000000u);
        }

        DrawText(interuiregular18, (uint32_t)(SETTINGS_LIST_X + 28.0f),
                 (uint32_t)(rcy + fontBaselineOffset(interuiregular18)),
                 focused ? txt : makeColorMuted(txt, dark ? 220 : 210), settingLabel(id));

        char rawVal[160];
        formatSettingValue(id, rawVal, sizeof(rawVal));
        char valBuf[192];
        if (focused && id != SET_RESET_DEFAULTS)
            snprintf(valBuf, sizeof(valBuf), "\u25C2  %s  \u25B8", rawVal);
        else
            snprintf(valBuf, sizeof(valBuf), "%s", rawVal);

        uint32_t vw = 0, vh = 0;
        GetTextDimensions(interuiregular18, valBuf, &vw, &vh);
        uint32_t max_vw = 360;
        if (vw > max_vw) vw = max_vw;
        color_t val_col = focused ? accent : muted;
        if (id == SET_RESET_DEFAULTS && s_reset_armed)
            val_col = MakeColor(245, 76, 86, 255);

        float val_x = SETTINGS_LIST_X + SETTINGS_LIST_W - 26.0f - (float)vw;
        DrawTextTruncate(interuiregular18, (uint32_t)val_x,
                         (uint32_t)(rcy + fontBaselineOffset(interuiregular18)), val_col, valBuf, max_vw, "...");

        /* Color swatch dot next to Accent / Custom RGB rows */
        if (id == SET_ACCENT_PRESET && s_cfg.accent_preset != ACCENT_IRIDESCENT) {
            renderCircleF(val_x - 16.0f, rcy, 8.0f, accent.abgr | 0xff000000u);
        } else if (id == SET_CUSTOM_R || id == SET_CUSTOM_G || id == SET_CUSTOM_B) {
            uint32_t bar_col = (id == SET_CUSTOM_R) ? makeRGBA(235, 68, 78, 255) :
                               (id == SET_CUSTOM_G) ? makeRGBA(68, 215, 118, 255) :
                                                      makeRGBA(68, 152, 245, 255);
            int val_i = (id == SET_CUSTOM_R) ? s_cfg.custom_r : (id == SET_CUSTOM_G ? s_cfg.custom_g : s_cfg.custom_b);
            float bx = val_x - 140.0f;
            renderRoundedRectF(bx, rcy - 4.0f, 120.0f, 8.0f, 4.0f, sep_col);
            renderRoundedRectF(bx, rcy - 4.0f, 120.0f * ((float)val_i / 255.0f), 8.0f, 4.0f, bar_col);
        }
    }

    /* Footer bar */
    renderRectF(42.0f, 644.0f, SCREEN_W - 84.0f, 1.0f, sep_col);
    DrawText(interuiregular14, 64, 680, muted, "L / R  Switch Category");

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

    s_app_pop_vel += (1.0f - s_app_pop) * 0.24f;
    s_app_pop_vel *= 0.62f;
    s_app_pop += s_app_pop_vel;

    s_pill_pop_vel += (1.0f - s_pill_pop) * 0.26f;
    s_pill_pop_vel *= 0.60f;
    s_pill_pop += s_pill_pop_vel;

    if (s_view != s_last_view) {
        s_last_view = s_view;
        triggerTransition();
    }
    if (s_trans_t < 1.0f) {
        s_trans_t += 0.068f;
        if (s_trans_t > 1.0f) s_trans_t = 1.0f;
    }

    /* Advance Switch 2 launch animation (~200ms = 12 frames at 60fps) */
    if (s_launching_entry) {
        s_launch_t += 0.085f;
        if (s_launch_t >= 1.0f) {
            menuEntry_s *target = s_launching_entry;
            s_launching_entry = NULL;
            s_launch_t = 0.0f;
            s_launch_blackout = true;
            launchMenuEntryTask(target);
            if (!menuIsMsgBoxOpen()) {
                /* Still launching cleanly: keep blackout veil until hbmenu exits */
            } else {
                /* Launch failed and opened an error dialog: lift blackout */
                s_launch_blackout = false;
                switch2BgmResetFade();
            }
            syncSelectionFromMenu(menuGetCurrent());
        }
    }
}

/* --- Public Entry Points -------------------------------------------------- */

static void drawTransitionVeil(void) {
    if (s_launch_blackout) {
        renderRectF(0.0f, 0.0f, SCREEN_W, SCREEN_H, makeRGBA(0, 0, 0, 255));
        return;
    }
    if (s_launching_entry) {
        uint8_t a = (uint8_t)(s_launch_t * 255.0f);
        renderRectF(0.0f, 0.0f, SCREEN_W, SCREEN_H, makeRGBA(0, 0, 0, a));
        return;
    }
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

    /* Base Switch 2 background */
    renderRectF(0.0f, 0.0f, SCREEN_W, SCREEN_H, themeCurrent.backgroundColor.abgr | 0xff000000u);

    /* Custom SD wallpaper (priority) or .romfs theme background image */
    if (switch2WallpaperIsLoaded()) {
        switch2WallpaperDraw(s_cfg.wallpaper_dim);
    } else {
        ThemeLayoutObject *bgObj = &themeCurrent.layoutObjects[ThemeLayoutId_BackgroundImage];
        assetsDataEntry *bgData = NULL;
        assetsGetData(AssetId_background_image, &bgData);
        if (bgObj->visible && bgData && bgData->buffer) {
            enum RenderImageFormat fmt = (bgData->imageMode == IMAGE_MODE_RGB24) ? RenderImageFormat_Rgb24 : RenderImageFormat_Rgba32;
            renderImageScaledF(bgData->buffer, bgData->imageSize[0], bgData->imageSize[1],
                               0.0f, 0.0f, SCREEN_W, SCREEN_H, fmt, 0xffffffffu);
        }
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

    /* Lock input during 200ms launch animation */
    if (s_launching_entry)
        return false;

    if (s_exit_requested) {
        s_exit_requested = false;
        return true;
    }

    /* Full-screen Switch 2 Sidebar Settings navigation */
    if (s_view == S2View_Settings) {
        if (down & HidNpadButton_Plus)
            return true;
        if (down & HidNpadButton_B) {
            s_reset_armed = false;
            switch2SfxPlay(S2Sfx_Back);
            s_view = s_prev_view;
            s_focus = S2Focus_Pill;
            return false;
        }
        if (down & HidNpadButton_L) {
            s_settings_cat = (S2SettingsCat)(((int)s_settings_cat + S2Cat_Count - 1) % S2Cat_Count);
            s_settings_sel = 0;
            s_settings_scroll = 0;
            s_reset_armed = false;
            switch2SfxPlay(S2Sfx_Pill);
            return false;
        }
        if (down & HidNpadButton_R) {
            s_settings_cat = (S2SettingsCat)(((int)s_settings_cat + 1) % S2Cat_Count);
            s_settings_sel = 0;
            s_settings_scroll = 0;
            s_reset_armed = false;
            switch2SfxPlay(S2Sfx_Pill);
            return false;
        }

        int items[16];
        int count = getCategoryItems(s_settings_cat, items);
        if (s_settings_sel >= count) s_settings_sel = count > 0 ? count - 1 : 0;

        if (down & HidNpadButton_AnyUp) {
            if (s_settings_sel > 0) {
                s_settings_sel--;
                s_reset_armed = false;
                switch2SfxPlay(S2Sfx_Pill);
            }
        } else if (down & HidNpadButton_AnyDown) {
            if (s_settings_sel < count - 1) {
                s_settings_sel++;
                s_reset_armed = false;
                switch2SfxPlay(S2Sfx_Pill);
            }
        } else if (down & HidNpadButton_A) {
            if (count > 0) changeSettingById(items[s_settings_sel], +1, true);
        } else if (down & HidNpadButton_AnyRight) {
            if (count > 0) changeSettingById(items[s_settings_sel], +1, false);
        } else if (down & HidNpadButton_AnyLeft) {
            if (count > 0) changeSettingById(items[s_settings_sel], -1, false);
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

    if (menuIsNetloaderActive()) {
        if (down & HidNpadButton_B) {
            switch2SfxPlay(S2Sfx_Back);
            launchMenuBackTask();
        } else if (down & HidNpadButton_Plus) {
            return true;
        }
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

    /* Pill bar navigation */
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

    /* App list navigation */
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
            if (s_sel == s_visible_count && hasEndAllSoftwareTile()) {
                switch2SfxPlay(S2Sfx_Confirm);
                s_view = S2View_Grid;
                s_sel = s_visible_count - 1;
            } else if (s_sel < s_visible_count) {
                activateEntry(menu, s_sel);
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
    if (s_launching_entry)
        return;

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
            if (!s_touch_is_tap && !menuIsMsgBoxOpen()) {
                if (s_view == S2View_Carousel) {
                    /* Audit Fix #3: clamp horizontal drag to [0, max_scroll] */
                    float max_s = carouselMaxScroll();
                    s_scroll_x -= (float)dx;
                    s_touch_vel_x = (float)dx;
                    if (s_scroll_x < 0.0f) s_scroll_x = 0.0f;
                    if (s_scroll_x > max_s) s_scroll_x = max_s;
                } else if (s_view == S2View_Grid) {
                    /* Audit Fix #7: vertical touch drag in All Software Grid */
                    float max_y = gridMaxScrollY();
                    s_grid_scroll_y -= (float)dy;
                    if (s_grid_scroll_y < 0.0f) s_grid_scroll_y = 0.0f;
                    if (s_grid_scroll_y > max_y) s_grid_scroll_y = max_y;
                }
            }
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

        /* Settings view tap handling (sidebar + rows + Audit Fix #2 footer split) */
        if (s_view == S2View_Settings) {
            /* Left sidebar categories */
            if (x1 >= (int)SETTINGS_SIDEBAR_X && x1 <= (int)(SETTINGS_SIDEBAR_X + SETTINGS_SIDEBAR_W)) {
                for (int c = 0; c < S2Cat_Count; c++) {
                    float cy = SETTINGS_CAT_Y0 + (float)c * (SETTINGS_CAT_H + SETTINGS_CAT_GAP);
                    if (y1 >= (int)cy && y1 <= (int)(cy + SETTINGS_CAT_H)) {
                        if ((int)s_settings_cat != c) {
                            s_settings_cat = (S2SettingsCat)c;
                            s_settings_sel = 0;
                            s_settings_scroll = 0;
                            s_reset_armed = false;
                            switch2SfxPlay(S2Sfx_Pill);
                        }
                        return;
                    }
                }
            }

            /* Right settings rows */
            int items[16];
            int count = getCategoryItems(s_settings_cat, items);
            if (x1 >= (int)SETTINGS_LIST_X && x1 <= (int)(SETTINGS_LIST_X + SETTINGS_LIST_W)) {
                int vis = count - s_settings_scroll;
                if (vis > SETTINGS_MAX_ROWS) vis = SETTINGS_MAX_ROWS;
                for (int row = 0; row < vis; row++) {
                    float ry = SETTINGS_LIST_Y + (float)row * SETTINGS_ROW_H;
                    if (y1 >= (int)ry && y1 <= (int)(ry + SETTINGS_ROW_H)) {
                        int idx = s_settings_scroll + row;
                        s_settings_sel = idx;
                        changeSettingById(items[idx], +1, true);
                        return;
                    }
                }
            }

            /* Audit Fix #2: distinguish A Change (right) vs B Back (left of it / header) */
            if (y1 >= 644 && x1 >= 1110) {
                if (count > 0 && s_settings_sel < count)
                    changeSettingById(items[s_settings_sel], +1, true);
                return;
            }
            if ((y1 >= 644 && x1 >= 940 && x1 < 1110) || (x1 < 344 && y1 < 92)) {
                s_reset_armed = false;
                switch2SfxPlay(S2Sfx_Back);
                s_view = s_prev_view;
                s_focus = S2Focus_Pill;
            }
            return;
        }

        /* Message box tap (Audit Fix #9: play SFX) */
        if (menuIsMsgBoxOpen()) {
            ThemeLayoutObject *sep = &themeCurrent.layoutObjects[ThemeLayoutId_MsgBoxSeparator];
            MessageBox mb = menuGetCurrentMsgBox();
            int sx = 1280 / 2 - mb.width / 2;
            int sy = (720 / 2 - mb.height / 2) + mb.height + sep->posStart[1];
            int ex = sx + mb.width;
            int ey = (720 / 2 - mb.height / 2) + mb.height;
            if (x1 > sx && x1 < ex && y1 > sy && y1 < ey) {
                if (menuIsConfirmMsgBoxOpen() && x1 < sx + (int)mb.width / 2) {
                    switch2SfxPlay(S2Sfx_Confirm);
                    menuConfirmMsgBox();
                } else {
                    switch2SfxPlay(S2Sfx_Back);
                    menuCloseMsgBox();
                }
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

        /* Carousel / Grid tile tap (Audit Fix #9: play SFX + activateEntry) */
        rebuildVisibleList(menu);
        if (s_view == S2View_Carousel) {
            if (y1 >= (int)CAROUSEL_TILE_Y && y1 <= (int)(CAROUSEL_TILE_Y + CAROUSEL_TILE_SZ)) {
                int total_slots = s_visible_count + (hasEndAllSoftwareTile() ? 1 : 0);
                for (int i = 0; i < total_slots; i++) {
                    float tx = CAROUSEL_START_X + (float)i * CAROUSEL_STRIDE - s_scroll_x;
                    if (x1 >= (int)tx && x1 <= (int)(tx + CAROUSEL_TILE_SZ)) {
                        if (s_focus == S2Focus_Apps && s_sel == i) {
                            if (i == s_visible_count) {
                                switch2SfxPlay(S2Sfx_Confirm);
                                s_view = S2View_Grid;
                                s_sel = s_visible_count - 1;
                            } else {
                                activateEntry(menu, i);
                            }
                        } else {
                            switch2SfxPlay(S2Sfx_Move);
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
                            activateEntry(menu, i);
                        } else {
                            switch2SfxPlay(S2Sfx_Move);
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
