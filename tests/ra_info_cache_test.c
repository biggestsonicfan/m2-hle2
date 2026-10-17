/*
 * ra_info_cache_test.c -- reading RetroArch's core info cache (core/ra_info_cache.h),
 * which the libretro core checks for a stale "no rewind" entry (Pinboard #587).
 *
 *  (A) The level, from the compact JSON RetroArch 1.18 writes and from an
 *      indented one; the right entry among several; no entry.
 *  (B) An rzip file of two chunks, inflated, as RetroArch writes it.
 *  (C) A cache in a folder, and the refresh marker left beside it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_info_cache.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

/* What RetroArch 1.18 wrote for the core while its .info said "basic". */
static const char k_compact[] =
    "{\"version\":\"1.2\",\"items\":[{\"display_name\":\"Other\",\"core_file_id\":{\"str\":\"other_libretro\","
    "\"hash\":1},\"savestate_support_level\":3,\"has_info\":true},{\"display_name\":\"Sega - Model 2 (m2-hle)\","
    "\"core_file_id\":{\"str\":\"m2hle_libretro\",\"hash\":655174179},\"firmware_count\":0,"
    "\"savestate_support_level\":1,\"has_info\":true}]}";

static const char k_indented[] =
    "{\n  \"version\": \"1.2\",\n  \"items\": [\n    {\n      \"display_name\": \"Sega - Model 2 (m2-hle)\",\n"
    "      \"core_file_id\": {\n        \"str\": \"m2hle_libretro\",\n        \"hash\": 655174179\n      },\n"
    "      \"savestate_support_level\": 2\n    },\n    {\n      \"display_name\": \"Next\",\n"
    "      \"savestate_support_level\": 0\n    }\n  ]\n}\n";

/* A core with no level of its own must not take the next entry's. */
static const char k_no_level[] =
    "{\"items\":[{\"display_name\":\"A\",\"core_file_id\":{\"str\":\"m2hle_libretro\",\"hash\":1}},"
    "{\"display_name\":\"B\",\"savestate_support_level\":3}]}";

static void test_levels(void) {
    CHECK(ra_info_cache_level(k_compact, strlen(k_compact), "m2hle_libretro") == RA_SAVESTATE_BASIC,
          "(A) compact: the stale entry reads BASIC");
    CHECK(ra_info_cache_level(k_compact, strlen(k_compact), "other_libretro") == RA_SAVESTATE_DETERMINISTIC,
          "(A) compact: another core's entry is its own");
    CHECK(ra_info_cache_level(k_compact, strlen(k_compact), "m2hle") == -1,
          "(A) a prefix of the id is not the id");
    CHECK(ra_info_cache_level(k_indented, strlen(k_indented), "m2hle_libretro") == RA_SAVESTATE_SERIALIZED,
          "(A) indented: SERIALIZED");
    CHECK(ra_info_cache_level(k_no_level, strlen(k_no_level), "m2hle_libretro") == -1,
          "(A) no level in the entry: -1, not the next entry's");
    CHECK(ra_info_cache_level(k_compact, strlen(k_compact), "absent_libretro") == -1, "(A) no entry: -1");
}

/* k_compact as an rzip file of two chunks. */
static uint8_t *make_rzip(size_t *n) {
    size_t total = strlen(k_compact), half = total / 2;
    uint8_t *b = (uint8_t *)calloc(1, 20 + 2 * (4 + 1024));
    memcpy(b, "#RZIPv\x01#", 8);
    b[8] = (uint8_t)half; b[9] = (uint8_t)(half >> 8);
    for (int i = 0; i < 8; i++) b[12 + i] = (uint8_t)((uint64_t)total >> (8 * i));
    size_t p = 20;
    for (int c = 0; c < 2; c++) {
        mz_ulong len = 1024;
        const char *src = k_compact + (c ? half : 0);
        mz_compress(b + p + 4, &len, (const unsigned char *)src, c ? total - half : half);
        for (int i = 0; i < 4; i++) b[p + i] = (uint8_t)(len >> (8 * i));
        p += 4 + len;
    }
    *n = p;
    return b;
}

static void test_rzip(void) {
    size_t n = 0, jn = 0;
    uint8_t *b = make_rzip(&n);
    char *json = ra_info_cache_json(b, n, &jn);
    CHECK(json && jn == strlen(k_compact) && !memcmp(json, k_compact, jn), "(B) two rzip chunks inflate to the JSON");
    free(json);
    b[24] ^= 0xFF;   /* break the first chunk's zlib header */
    CHECK(ra_info_cache_json(b, n, &jn) == NULL, "(B) a broken chunk is refused");
    free(b);
}

static void test_folder(void) {
    char dir[] = "/tmp/ra_info_cache_test_XXXXXX", path[256];
#ifdef _WIN32
    snprintf(dir, sizeof dir, "%s", ".");
#else
    if (!mkdtemp(dir)) { CHECK(0, "(C) mkdtemp"); return; }
#endif
    CHECK(ra_info_cache_dir_level(dir, "m2hle_libretro") == -1, "(C) no cache file: -1");
    size_t n = 0;
    uint8_t *b = make_rzip(&n);
    snprintf(path, sizeof path, "%s/%s", dir, RA_INFO_CACHE_FILE);
    FILE *f = fopen(path, "wb");
    fwrite(b, 1, n, f);
    fclose(f);
    free(b);
    CHECK(ra_info_cache_dir_level(dir, "m2hle_libretro") == RA_SAVESTATE_BASIC, "(C) the folder's cache reads BASIC");
    CHECK(ra_info_cache_mark_refresh(dir), "(C) the refresh marker is written");
    remove(path);
    snprintf(path, sizeof path, "%s/%s", dir, RA_INFO_REFRESH_FILE);
    f = fopen(path, "rb");
    CHECK(f != NULL, "(C) the marker is where RetroArch looks");
    if (f) fclose(f);
    remove(path);
#ifndef _WIN32
    remove(dir);
#endif
}

int main(void) {
    test_levels();
    test_rzip();
    test_folder();
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
