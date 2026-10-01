/*
 * test_rom_dir.h — where the tests that load STF find sfight.zip and schamp.zip.
 *
 *   $ROMDIR      an explicit directory (what snd_replay always took)
 *   $ROMS_DIR    the machine's one ROM folder (the dev container sets it:
 *                every stock set it has, as symlinks; antigravity-dev-docker)
 *   otherwise    the claude_mame oracle's roms on the owner's Windows machine
 *
 * Never copy a set to make a test find it: point one of these at it.
 */
#ifndef TEST_ROM_DIR_H
#define TEST_ROM_DIR_H

#include <stdio.h>
#include <stdlib.h>

static const char *test_rom_dir(void) {
    const char *d = getenv("ROMDIR");
    if (!d || !d[0]) d = getenv("ROMS_DIR");
    if (!d || !d[0]) d = "c:/Users/bigge/source/repos/ai/claude_mame/mame/roms";
    return d;
}

/* <dir>/<name>. Four buffers in turn, so two calls fit in one argument list. */
static const char *test_rom(const char *name) {
    static char buf[4][1024];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, sizeof buf[0], "%s/%s", test_rom_dir(), name);
    return b;
}

#endif
