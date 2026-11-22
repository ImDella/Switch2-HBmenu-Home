/* SPDX-License-Identifier: ISC
 * Lightweight procedural Switch 2 tactile UI SFX engine (non-blocking audout).
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
    S2Sfx_Count = 5,
} S2SfxId;

void switch2SfxInit(void);
void switch2SfxExit(void);
void switch2SfxSetEnabled(bool enabled);
void switch2SfxPlay(S2SfxId id);

#endif
