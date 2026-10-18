/*
 * region_test.c -- the board region (g_region, core/hle_hooks.h) on every
 * runnable profile, on a bare memory bus. No ROM.
 *
 * Every profile in registry.h says what it does with the region
 * (quirks.region, game_region_use_t). A hooked one names two hooks in its table:
 * the factory default's and the one after a saved block is accepted, which
 * runs here on every hooked profile: a saved country stays when the region was
 * not asked for and becomes g_region when it was (g_region_over_saved). The web
 * page sets the region from its ?lang= (Pinboard #942), and a kept save must
 * not hide it (#948), so a new profile cannot skip that hook: the other kinds
 * need a reason and are only taken by the profiles listed here, each as its
 * listed kind.
 *
 * A ctest (CMakeLists.txt, region_test).
 */
#define NDEBUG 1
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "memory.h"
#include "i960.h"
#include "emu_thread.h"
#include "registry.h"

static memory_bus_t bus;
static i960_cpu_t   cpu;
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } \
                              else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static hle_hook_fn hook_named(const game_profile_t *p, const char *name) {
    for (int i = 0; i < p->hook_count; i++)
        if (p->hooks[i].name && !strcmp(p->hooks[i].name, name)) return p->hooks[i].fn;
    return NULL;
}

/* The only profiles that take no region hooks, each with the kind it may
 * declare. A profile not here must be REGION_HOOKED; add one only when the set
 * truly has no country setting (homebrew, a one-country program ROM), never
 * for "not found yet". */
static const struct { const char *id; game_region_kind_t kind; } g_region_unhooked[] = {
    { "dynamcop",         REGION_TODO },
    { "dyndeka2",         REGION_TODO },
    { "dynamcopb",        REGION_TODO },
    { "dyndeka2b",        REGION_TODO },
    { "dynamcopc",        REGION_TODO },
    { "doa",              REGION_TODO },
    { "doab",             REGION_TODO },
    { "doaa",             REGION_TODO },
    { "doaab",            REGION_TODO },
    { "doaae",            REGION_PROGRAM_ROM },
    { "airwlkrs",         REGION_PROGRAM_ROM },
    { "vonj",            REGION_PROGRAM_ROM },
    { "powsled",         REGION_TODO },
    { "daytona",         REGION_TODO },
    { "m2snake",         REGION_HOMEBREW },
    { "sfight_homebrew", REGION_HOMEBREW },
};

static int region_unhooked_allowed(const char *id, game_region_kind_t kind) {
    for (size_t i = 0; i < sizeof g_region_unhooked / sizeof g_region_unhooked[0]; i++)
        if (!strcmp(g_region_unhooked[i].id, id)) return g_region_unhooked[i].kind == kind;
    return 0;
}

static uint32_t hook_addr(const game_profile_t *p, const char *name) {
    for (int i = 0; i < p->hook_count; i++)
        if (p->hooks[i].name && !strcmp(p->hooks[i].name, name)) return p->hooks[i].addr;
    return 0;
}

/* A saved country (EXPORT, or JAPAN when that is asked for) under each region. */
static void check_over_saved(const game_profile_t *p, hle_hook_fn fn) {
    int ok_kept = 1, ok_over = 1;
    for (int r = GAME_REGION_JAPAN; r <= GAME_REGION_EXPORT; r++) {
        uint8_t saved = r == GAME_REGION_EXPORT ? GAME_REGION_JAPAN : GAME_REGION_EXPORT;
        uint32_t at = p->quirks.region.country(&bus);
        g_region = r;
        g_region_over_saved = 0;
        mem_write8(&bus, at, saved);
        ok_kept &= fn(&cpu, &bus) == 1 && mem_read8(&bus, at) == saved;
        g_region_over_saved = 1;
        ok_over &= fn(&cpu, &bus) == 1 && mem_read8(&bus, at) == (uint32_t)r;
    }
    g_region_over_saved = 0;
    CHECK(ok_kept, "%s: a saved country stays when the region was not asked for", p->id);
    CHECK(ok_over, "%s: the asked-for region goes over the saved country", p->id);
}

static void check_hooked(const game_profile_t *p) {
    const game_region_use_t *g = &p->quirks.region;
    hle_hook_fn def  = g->factory_default ? hook_named(p, g->factory_default) : NULL;
    hle_hook_fn over = g->over_saved      ? hook_named(p, g->over_saved)      : NULL;
    CHECK(def != NULL, "%s: the factory default's hook is in the table", p->id);
    CHECK(over != NULL, "%s: the hook over a saved block is in the table", p->id);
    CHECK(g->country != NULL, "%s: country() says where the working copy's country is", p->id);
    if (!def || !over || !g->country) return;
    CHECK(hook_addr(p, g->factory_default) != hook_addr(p, g->over_saved),
          "%s: the two hooks are at two places", p->id);
    check_over_saved(p, over);
}

static void check_declared(void) {
    for (size_t i = 0; i < g_profile_count; i++) {
        const game_profile_t *p = g_profiles[i];
        const game_region_use_t *g = &p->quirks.region;
        if (g->kind == REGION_HOOKED) { check_hooked(p); continue; }
        CHECK(g->kind != REGION_UNDECLARED, "%s: says what it does with the region", p->id);
        CHECK(g->why && g->why[0], "%s: says why it has no region hook", p->id);
        CHECK(!g->factory_default && !g->over_saved, "%s: names no hooks it does not take", p->id);
        CHECK(region_unhooked_allowed(p->id, g->kind), "%s: on the list of profiles without hooks, as that kind", p->id);
    }
}

/* Sega Rally's factory default writes COUNTRY itself (STF's loads r15). */
static void check_srallyc(void) {
    hle_hook_fn fn = hook_named(&srallyc_profile, "country_default");
    CHECK(fn != NULL, "srallyc: country_default hook");
    if (!fn) return;
    for (int r = GAME_REGION_JAPAN; r <= GAME_REGION_EXPORT; r++) {
        g_region = r;
        mem_write8(&bus, 0x00202019u, 0);
        int run = fn(&cpu, &bus);
        CHECK(run == 1 && mem_read8(&bus, 0x00202019u) == (uint32_t)r,
              "srallyc: region %d -> COUNTRY %d, the instruction runs", r, r);
    }
}

static void check_sfight(void) {
    CHECK(hook_named(&sfight_console_profile, "settings_loaded") == hook_named(&sfight_profile, "settings_loaded"),
          "sfight_console: the same settings_loaded hook");
    mem_write32(&bus, SFIGHT_SETTINGS_PTR, 0x00599000u);
    CHECK(sfight_country(&bus) == SFIGHT_SETTINGS_WORK + 0x12u, "sfight: country_val in the working block");
    check_over_saved(&sfight_profile, sfight_hook_settings_loaded);
}

int main(void) {
    if (!mem_init(&bus, NULL, 0)) { printf("mem_init failed\n"); return 1; }
    i960_reset(&cpu);
    check_declared();
    check_srallyc();
    check_sfight();
    printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
