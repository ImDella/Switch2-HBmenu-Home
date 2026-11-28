/* SPDX-License-Identifier: ISC
 * Virtual Groups (Switch 2 Groups / virtual folders) for Switch2-HBmenu-Home.
 * Stored in sdmc:/config/nx-hbmenu/switch2_groups.cfg (libconfig format).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libconfig.h>

#include "switch2_groups.h"

typedef struct {
    char name[S2_GROUP_NAME_MAX];
    char apps[S2_MAX_APPS_PER_GROUP][PATH_MAX];
    int app_count;
} S2Group;

static S2Group s_groups[S2_MAX_GROUPS];
static int s_group_count = 0;

static void getGroupsPath(char *out, size_t out_size) {
    snprintf(out, out_size, "%s/config/nx-hbmenu/switch2_groups.cfg", menuGetRootBasePath());
}

void switch2GroupsLoad(void) {
    s_group_count = 0;
    config_t cfg;
    config_init(&cfg);

    char path[PATH_MAX + 64];
    getGroupsPath(path, sizeof(path));
    if (config_read_file(&cfg, path) != CONFIG_TRUE) {
        config_destroy(&cfg);
        return;
    }

    config_setting_t *list = config_lookup(&cfg, "groups");
    if (list && config_setting_is_list(list)) {
        int n = config_setting_length(list);
        if (n > S2_MAX_GROUPS) n = S2_MAX_GROUPS;
        for (int i = 0; i < n; i++) {
            config_setting_t *grp = config_setting_get_elem(list, (unsigned int)i);
            if (!grp) continue;
            const char *name = NULL;
            if (!config_setting_lookup_string(grp, "name", &name) || !name || !name[0])
                continue;

            S2Group *dst = &s_groups[s_group_count];
            memset(dst, 0, sizeof(*dst));
            snprintf(dst->name, sizeof(dst->name), "%s", name);

            config_setting_t *apps = config_setting_lookup(grp, "apps");
            if (apps && (config_setting_is_array(apps) || config_setting_is_list(apps))) {
                int m = config_setting_length(apps);
                if (m > S2_MAX_APPS_PER_GROUP) m = S2_MAX_APPS_PER_GROUP;
                for (int j = 0; j < m; j++) {
                    const char *app_path = config_setting_get_string_elem(apps, j);
                    if (app_path && app_path[0]) {
                        snprintf(dst->apps[dst->app_count], PATH_MAX, "%s", app_path);
                        dst->app_count++;
                    }
                }
            }
            s_group_count++;
        }
    }

    config_destroy(&cfg);
}

void switch2GroupsSave(void) {
    config_t cfg;
    config_init(&cfg);
    config_setting_t *root = config_root_setting(&cfg);
    config_setting_t *list = config_setting_add(root, "groups", CONFIG_TYPE_LIST);

    if (list) {
        for (int i = 0; i < s_group_count; i++) {
            config_setting_t *grp = config_setting_add(list, NULL, CONFIG_TYPE_GROUP);
            if (!grp) continue;
            config_setting_t *nm = config_setting_add(grp, "name", CONFIG_TYPE_STRING);
            if (nm) config_setting_set_string(nm, s_groups[i].name);

            config_setting_t *apps = config_setting_add(grp, "apps", CONFIG_TYPE_ARRAY);
            if (apps) {
                for (int j = 0; j < s_groups[i].app_count; j++) {
                    config_setting_t *el = config_setting_add(apps, NULL, CONFIG_TYPE_STRING);
                    if (el) config_setting_set_string(el, s_groups[i].apps[j]);
                }
            }
        }
    }

    char path[PATH_MAX + 64];
    getGroupsPath(path, sizeof(path));
    config_write_file(&cfg, path);
    config_destroy(&cfg);
}

int switch2GroupsCount(void) {
    return s_group_count;
}

const char *switch2GroupName(int group_idx) {
    if (group_idx < 0 || group_idx >= s_group_count)
        return "";
    return s_groups[group_idx].name;
}

int switch2GroupCreate(const char *name) {
    if (!name || !name[0] || s_group_count >= S2_MAX_GROUPS)
        return -1;
    int idx = s_group_count++;
    memset(&s_groups[idx], 0, sizeof(S2Group));
    snprintf(s_groups[idx].name, sizeof(s_groups[idx].name), "%s", name);
    switch2GroupsSave();
    return idx;
}

void switch2GroupRename(int group_idx, const char *new_name) {
    if (group_idx < 0 || group_idx >= s_group_count || !new_name || !new_name[0])
        return;
    snprintf(s_groups[group_idx].name, sizeof(s_groups[group_idx].name), "%s", new_name);
    switch2GroupsSave();
}

void switch2GroupDelete(int group_idx) {
    if (group_idx < 0 || group_idx >= s_group_count)
        return;
    for (int i = group_idx; i < s_group_count - 1; i++) {
        s_groups[i] = s_groups[i + 1];
    }
    s_group_count--;
    switch2GroupsSave();
}

bool switch2GroupContains(int group_idx, const char *path) {
    if (group_idx < 0 || group_idx >= s_group_count || !path || !path[0])
        return false;
    const S2Group *g = &s_groups[group_idx];
    for (int i = 0; i < g->app_count; i++) {
        if (strcmp(g->apps[i], path) == 0)
            return true;
    }
    return false;
}

bool switch2GroupToggleApp(int group_idx, const char *path) {
    if (group_idx < 0 || group_idx >= s_group_count || !path || !path[0])
        return false;
    S2Group *g = &s_groups[group_idx];
    for (int i = 0; i < g->app_count; i++) {
        if (strcmp(g->apps[i], path) == 0) {
            for (int j = i; j < g->app_count - 1; j++)
                memcpy(g->apps[j], g->apps[j + 1], PATH_MAX);
            g->app_count--;
            switch2GroupsSave();
            return false; /* removed */
        }
    }
    if (g->app_count < S2_MAX_APPS_PER_GROUP) {
        snprintf(g->apps[g->app_count], PATH_MAX, "%s", path);
        g->app_count++;
        switch2GroupsSave();
        return true; /* added */
    }
    return false;
}

bool switch2GroupsHasAppInAny(const char *path) {
    if (!path || !path[0])
        return false;
    for (int i = 0; i < s_group_count; i++) {
        if (switch2GroupContains(i, path))
            return true;
    }
    return false;
}

int switch2GroupCountPresent(int group_idx, menu_s *menu) {
    if (group_idx < 0 || group_idx >= s_group_count || !menu)
        return 0;
    int count = 0;
    for (menuEntry_s *me = menu->firstEntry; me; me = me->next) {
        if (switch2GroupContains(group_idx, me->path))
            count++;
    }
    return count;
}

int switch2GroupCollectIcons(int group_idx, menu_s *menu, menuEntry_s **out_entries, int max_out) {
    if (group_idx < 0 || group_idx >= s_group_count || !menu || !out_entries || max_out <= 0)
        return 0;
    int count = 0;
    for (menuEntry_s *me = menu->firstEntry; me && count < max_out; me = me->next) {
        if (switch2GroupContains(group_idx, me->path))
            out_entries[count++] = me;
    }
    return count;
}
