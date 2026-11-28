/* SPDX-License-Identifier: ISC
 * Virtual Groups (Switch 2 Groups / virtual folders) for Switch2-HBmenu-Home.
 * Organizes homebrew apps in the UI without modifying SD card directory structure.
 */
#ifndef HBMENU_SWITCH2_GROUPS_H
#define HBMENU_SWITCH2_GROUPS_H

#include <stdbool.h>
#include "../common/common.h"

#define S2_MAX_GROUPS         32
#define S2_GROUP_NAME_MAX     64
#define S2_MAX_APPS_PER_GROUP 128

void switch2GroupsLoad(void);
void switch2GroupsSave(void);

int switch2GroupsCount(void);
const char *switch2GroupName(int group_idx);

int switch2GroupCreate(const char *name);
void switch2GroupRename(int group_idx, const char *new_name);
void switch2GroupDelete(int group_idx);

bool switch2GroupContains(int group_idx, const char *path);
bool switch2GroupToggleApp(int group_idx, const char *path);
bool switch2GroupsHasAppInAny(const char *path);

int switch2GroupCountPresent(int group_idx, menu_s *menu);
int switch2GroupCollectIcons(int group_idx, menu_s *menu, menuEntry_s **out_entries, int max_out);

#endif
