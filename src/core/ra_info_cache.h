/*
 * ra_info_cache.h -- RetroArch's core info cache, read to see what it says
 * about this core's save states (Pinboard #587).
 *
 * RetroArch keeps what every core's .info says in `core_info.cache`, beside
 * the .info files, and it keys an entry by the core's file name only: it
 * never reads a cached core's .info again. A cache written while our .info
 * said `savestate_features = "basic"` (#426) keeps saying so after the .info
 * says "serialized" (#585), and RetroArch then hides Settings > Frame Throttle
 * > Rewind and refuses the rewind hotkey ("this core lacks serialized save
 * state support"). Nothing a core can tell RetroArch changes that.
 *
 * What RetroArch does offer is a marker: a file named `core_info.refresh` in
 * the same folder makes its next start throw the cache away and read every
 * .info again (core_info.c, core_info_cache_read). So the core reads the
 * cache, and if its own entry is below what it needs, it leaves the marker.
 * RetroArch 1.18 reads its core info again once a game has loaded, which takes
 * the marker: Rewind is in the menu and the hotkey works in that same session
 * (measured). An older RetroArch may need a restart, so the core says so.
 *
 * The cache is JSON, rzip-compressed by most builds: "#RZIPv\x01#", a u32
 * chunk size, a u64 total size, then chunks of [u32 length][zlib stream]
 * (libretro-common's interface_stream/rzip_stream.c). Only the one field is
 * read, by text, as the cache's own writer lays it out (compact or indented).
 */
#ifndef RA_INFO_CACHE_H
#define RA_INFO_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

/* RetroArch's levels (core_info.h). Rewind needs SERIALIZED. */
#define RA_SAVESTATE_DISABLED      0
#define RA_SAVESTATE_BASIC         1
#define RA_SAVESTATE_SERIALIZED    2
#define RA_SAVESTATE_DETERMINISTIC 3

#define RA_INFO_CACHE_FILE   "core_info.cache"
#define RA_INFO_REFRESH_FILE "core_info.refresh"
#define RA_INFO_CACHE_MAX    (16u << 20)   /* a cache of every core is ~1 MB */

static const char *ra__skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *ra__find(const char *p, const char *end, const char *needle, size_t len) {
    for (; p + len <= end; p++)
        if (*p == *needle && !memcmp(p, needle, len)) return p;
    return NULL;
}

/* The "savestate_support_level" number between p and stop, or -1. */
static int ra__entry_level(const char *p, const char *stop) {
    static const char lvl[] = "\"savestate_support_level\"";
    const char *q = ra__find(p, stop, lvl, sizeof lvl - 1);
    if (!q) return -1;
    q = ra__skip_ws(q + sizeof lvl - 1, stop);
    if (q >= stop || *q != ':') return -1;
    q = ra__skip_ws(q + 1, stop);
    return q < stop && *q >= '0' && *q <= '9' ? *q - '0' : -1;
}

/* The save-state level the cache's JSON gives the core whose file id is `id`
 * (the core's file name without its extension, "m2hle_libretro"), or -1 when
 * the cache has no entry for it. The id is the "str" of the entry's
 * "core_file_id" (as a value: a ':' after it would make it a key), and the
 * entry runs to the next item's "display_name", which every entry starts with. */
static int ra_info_cache_level(const char *json, size_t n, const char *id) {
    char key[96];
    const char *end = json + n, *p = json;
    int klen = snprintf(key, sizeof key, "\"%s\"", id);
    if (klen <= 2 || (size_t)klen >= sizeof key) return -1;
    while ((p = ra__find(p, end, key, (size_t)klen)) != NULL) {
        p += klen;
        const char *after = ra__skip_ws(p, end);
        if (after < end && *after == ':') continue;
        const char *stop = ra__find(p, end, "\"display_name\"", 14);
        return ra__entry_level(p, stop ? stop : end);
    }
    return -1;
}

/* A cache file's bytes as JSON: rzip chunks inflated, or the bytes as they
 * are. Returns a malloc'd buffer (the caller frees it) or NULL. */
static char *ra_info_cache_json(const uint8_t *b, size_t n, size_t *out_n) {
    static const uint8_t magic[8] = { '#', 'R', 'Z', 'I', 'P', 'v', 1, '#' };
    *out_n = 0;
    if (n < 20 || memcmp(b, magic, 8)) {
        char *s = (char *)malloc(n + 1);
        if (!s) return NULL;
        memcpy(s, b, n); s[n] = 0;
        *out_n = n;
        return s;
    }
    uint32_t chunk = (uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24;
    uint64_t total = 0;
    for (int i = 0; i < 8; i++) total |= (uint64_t)b[12 + i] << (8 * i);
    if (!chunk || total > RA_INFO_CACHE_MAX) return NULL;
    char *s = (char *)malloc((size_t)total + 1);
    if (!s) return NULL;
    size_t p = 20, got = 0;
    while (p + 4 <= n && got < total) {
        uint32_t len = (uint32_t)b[p] | (uint32_t)b[p + 1] << 8 | (uint32_t)b[p + 2] << 16 | (uint32_t)b[p + 3] << 24;
        p += 4;
        mz_ulong room = (mz_ulong)(total - got);
        if (len > n - p || mz_uncompress((unsigned char *)s + got, &room, b + p, len) != MZ_OK) {
            free(s);
            return NULL;
        }
        got += room;
        p += len;
    }
    s[got] = 0;
    *out_n = got;
    return s;
}

/* The level `dir`'s cache gives core `id`, or -1 (no cache, unreadable, no
 * entry). */
static int ra_info_cache_dir_level(const char *dir, const char *id) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, RA_INFO_CACHE_FILE);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (n > 0 && n <= (long)RA_INFO_CACHE_MAX) ? (uint8_t *)malloc((size_t)n) : NULL;
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    size_t jn = 0;
    char *json = got == (size_t)n && b ? ra_info_cache_json(b, got, &jn) : NULL;
    free(b);
    int level = json ? ra_info_cache_level(json, jn, id) : -1;
    free(json);
    return level;
}

/* Leaves RetroArch's refresh marker in `dir`; false if it cannot. */
static bool ra_info_cache_mark_refresh(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, RA_INFO_REFRESH_FILE);
    FILE *f = fopen(path, "ab");
    if (!f) return false;
    fclose(f);
    return true;
}

#endif /* RA_INFO_CACHE_H */
