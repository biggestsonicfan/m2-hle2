/*
 * savestate_reg.h — state a game profile keeps outside the board's structs.
 *
 * A profile's own statics (the Console profile's hidden-select latch) are part
 * of what a savestate has to carry, but the savestate module (savestate.h)
 * sits below the profiles and cannot name them. A profile registers each one
 * from its install_fn instead; the savestate writes it as an entry of its own
 * (`P.<name>`) and reads it back into the same bytes. An entry belongs to the
 * profile that registered it and is only written or read while that profile
 * is the active one, so switching profiles needs no teardown; registering a
 * name again replaces it, so every install can register.
 */
#ifndef M2HLE_SAVESTATE_REG_H
#define M2HLE_SAVESTATE_REG_H

#include <stddef.h>
#include <string.h>

#define SAVESTATE_EXTRA_MAX 16

typedef struct {
    const char *profile;   /* game_profile_t.id of the owner */
    const char *name;
    void       *data;
    size_t      size;
} savestate_extra_t;

static savestate_extra_t g_savestate_extra[SAVESTATE_EXTRA_MAX];
static int               g_savestate_extra_n = 0;

static inline void savestate_extra(const char *profile, const char *name, void *data, size_t size) {
    for (int i = 0; i < g_savestate_extra_n; i++)
        if (strcmp(g_savestate_extra[i].profile, profile) == 0 && strcmp(g_savestate_extra[i].name, name) == 0) {
            g_savestate_extra[i].data = data;
            g_savestate_extra[i].size = size;
            return;
        }
    if (g_savestate_extra_n < SAVESTATE_EXTRA_MAX)
        g_savestate_extra[g_savestate_extra_n++] = (savestate_extra_t){ profile, name, data, size };
}

#endif /* M2HLE_SAVESTATE_REG_H */
