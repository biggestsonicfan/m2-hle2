/*
 * rom_loader.h — generic helpers + romset container.
 *
 * Game-specific loaders (see src/profiles/) use these helpers to fill a
 * romset_t — a flat intermediate buffer per ROM region (MAME-style: maincpu,
 * main_data, copro_data, polygons, textures, audiocpu, samples). The
 * romset is then handed to the profile's installer to copy/map data into
 * the active memory_bus_t.
 *
 * Helpers cover the load operations MAME drivers use for Model 2:
 *   - file_load                 : read a hacked override file from disk
 *   - zip_extract               : pull a named file out of a .zip via miniz
 *   - zip_extract_from_set      : try child zip first, then parent, verify CRC32
 *   - interleave_32_word        : ROM_LOAD32_WORD — two halves into 32-bit words
 *   - load_16_word_swap         : ROM_LOAD16_WORD_SWAP — swap 16-bit pairs
 *   - rom_region_copy           : ROM_COPY — mirror a window within a region
 */
#ifndef ROM_LOADER_H
#define ROM_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "miniz.h"

/* ---- CRC32 --------------------------------------------------------------- */

static uint32_t rl_crc32_tab[256];
static int      rl_crc32_tab_ready = 0;

static inline void rl_crc32_init(void) {
    if (rl_crc32_tab_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
        rl_crc32_tab[i] = c;
    }
    rl_crc32_tab_ready = 1;
}

static inline uint32_t rl_crc32(const uint8_t *data, size_t len) {
    rl_crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = rl_crc32_tab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- File / zip extraction ---------------------------------------------- */

static inline uint8_t *file_load(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) { free(data); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)sz;
    return data;
}

static inline uint8_t *zip_extract(const char *zippath, const char *filename, size_t *out_size) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zippath, 0)) return NULL;
    void *data = mz_zip_reader_extract_file_to_heap(&zip, filename, out_size,
                                                    MZ_ZIP_FLAG_IGNORE_PATH);
    mz_zip_reader_end(&zip);
    return (uint8_t *)data;
}

/* Try the child zip first (e.g. sfight.zip), fall back to the parent
 * (e.g. schamp.zip) for files shared via MAME's clone mechanism.
 * Verifies CRC32 — mismatches log a warning but the buffer is still
 * returned, since hacked / mod sets are intentionally a use case. */
static inline uint8_t *zip_extract_from_set(const char *child_zip, const char *parent_zip,
                                            const char *filename, size_t *out_size,
                                            uint32_t expected_crc) {
    uint8_t *data = NULL;
    if (child_zip)  data = zip_extract(child_zip,  filename, out_size);
    if (!data && parent_zip) data = zip_extract(parent_zip, filename, out_size);
    if (!data) { LOG_ERROR("ROM not found: %s", filename); return NULL; }

    uint32_t actual = rl_crc32(data, *out_size);
    if (actual != expected_crc) {
        LOG_WARN("CRC mismatch: %s (expected %08X, got %08X)", filename, expected_crc, actual);
    } else {
        LOG_DEBUG("ROM OK: %s (%zu bytes, CRC %08X)", filename, *out_size, actual);
    }
    return data;
}

/* ---- Transforms --------------------------------------------------------- */

/* MAME's ROM_LOAD32_WORD: interleave 16-bit halves into 32-bit words. */
static inline void interleave_32_word(uint8_t *dest, size_t dest_size,
                                      const uint8_t *lo, size_t lo_size,
                                      const uint8_t *hi, size_t hi_size,
                                      uint32_t base_offset) {
    size_t n = lo_size < hi_size ? lo_size : hi_size;
    for (size_t i = 0; i < n; i += 2) {
        uint32_t dst = base_offset + (uint32_t)(i * 2);
        if (dst + 3 >= dest_size) break;
        dest[dst + 0] = lo[i + 0];
        dest[dst + 1] = lo[i + 1];
        dest[dst + 2] = hi[i + 0];
        dest[dst + 3] = hi[i + 1];
    }
}

/* MAME's ROM_LOAD16_WORD_SWAP: byte-swap big-endian 16-bit data on the way in. */
static inline void load_16_word_swap(uint8_t *dest, size_t dest_size,
                                     const uint8_t *src, size_t src_size,
                                     uint32_t base_offset) {
    for (size_t i = 0; i + 1 < src_size && base_offset + i + 1 < dest_size; i += 2) {
        dest[base_offset + i + 0] = src[i + 1];
        dest[base_offset + i + 1] = src[i + 0];
    }
}

/* MAME's ROM_COPY: mirror a window from one offset to another inside the
 * same region (used by some Model 2 sets to repeat a smaller image). */
static inline void rom_region_copy(uint8_t *dest, size_t dest_size,
                                   uint32_t src_off, uint32_t dst_off, uint32_t len) {
    if (src_off + len <= dest_size && dst_off + len <= dest_size) {
        memcpy(&dest[dst_off], &dest[src_off], len);
    }
}

/* ---- romset container --------------------------------------------------- */

typedef struct romset {
    uint8_t *maincpu;     size_t maincpu_size;
    uint8_t *main_data;   size_t main_data_size;
    uint8_t *copro_data;  size_t copro_data_size;
    uint8_t *polygons;    size_t polygons_size;
    uint8_t *textures;    size_t textures_size;
    uint8_t *audiocpu;    size_t audiocpu_size;
    uint8_t *samples;     size_t samples_size;
    bool     loaded;
} romset_t;

static inline void romset_free(romset_t *rs) {
    free(rs->maincpu);
    free(rs->main_data);
    free(rs->copro_data);
    free(rs->polygons);
    free(rs->textures);
    free(rs->audiocpu);
    free(rs->samples);
    memset(rs, 0, sizeof(*rs));
}

#endif /* ROM_LOADER_H */
