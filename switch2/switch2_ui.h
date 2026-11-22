/* SPDX-License-Identifier: ISC
 * Switch 2 HOME Menu UI for ounce-hbmenu.
 */
#ifndef HBMENU_SWITCH2_UI_H
#define HBMENU_SWITCH2_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "../common/common.h"

void switch2UiInit(ThemePreset system_preset);
void switch2MenuDraw(void);
bool switch2MenuUpdate(u64 down);
void switch2HandleTouch(menu_s *menu);

#endif
