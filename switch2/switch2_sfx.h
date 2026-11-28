/* SPDX-License-Identifier: ISC
 * Real-time Switch 2 audio engine: procedural tactile SFX, ZIP sound packs,
 * and streaming BGM (WAV / MP3 / OGG) via audout.
 */
#ifndef HBMENU_SWITCH2_SFX_H
#define HBMENU_SWITCH2_SFX_H

#include <stdbool.h>

typedef enum {
    S2Sfx_Move = 0,     /* Soft tactile tick when moving between apps */
    S2Sfx_Pill = 1,     /* Crisp light tap on pill bar / settings list */
    S2Sfx_Confirm = 2,  /* Warm two-tone confirm chime on A / toggle */
    S2Sfx_Back = 3,     /* Soft downward mellow blip on B */
    S2Sfx_Star = 4,     /* Bright sparkle chime on Star / Unstar */
    S2Sfx_Launch = 5,   /* App launch chime */
    S2Sfx_Count = 6,
} S2SfxId;

void switch2SfxInit(void);
void switch2SfxExit(void);

/* SFX controls */
void switch2SfxSetEnabled(bool enabled);
void switch2SfxSetVolume(int volume_pct); /* 0..100 */
void switch2SfxPlay(S2SfxId id);

/* ZIP Sound Pack loader (pass NULL or "" for Built-in Switch 2 SFX) */
bool switch2SfxLoadPack(const char *zip_path);
bool switch2SfxPackHasBgm(void);

/* BGM streaming controls */
void switch2BgmSetVolume(int volume_pct); /* 0..100 */
bool switch2BgmPlayFile(const char *path);
bool switch2BgmPlayPack(const char *zip_path);
void switch2BgmStop(void);
void switch2BgmStartFadeOut(float duration_sec);
void switch2BgmResetFade(void);

#endif
