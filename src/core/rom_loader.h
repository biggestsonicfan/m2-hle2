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
 *   - romset_load_dir / _save_dir : a set as one straight image a region
 */
#ifndef ROM_LOADER_H
#define ROM_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "log.h"
#include "miniz.h"

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

/*
 * A ROM set held in memory instead of on disk: the web build is handed the bytes
 * of the zips the player picked and has no file system to put them in. While
 * g_rl_mem_zip is set, zip_extract_from_set ignores its zip paths and reads here.
 * It holds one zip, or a few: a split clone set is the clone's zip and its
 * parent's (vonj.zip and von.zip, Pinboard #940), and a file is looked for in
 * each of them, in the order they were given.
 *
 * Entries are found BY CRC32, out of the zip's central directory, which costs no
 * decompression. A merged MAME set is one zip holding a parent and its clones,
 * the clones' files usually in a subfolder, under whatever name the ROM manager
 * gave the zip -- so neither the zip's name nor an entry's path says what a file
 * is, and looking one up by base name returns whichever same-named entry comes
 * first. The CRC is the identity the profile already states for every file.
 *
 * `strict`: a file whose CRC is not in the zip fails the load rather than falling
 * back to its name with a warning. On disk a mismatch is a use case (hacked
 * sets); over netplay one different byte is a desync, so the web build is strict.
 */
#define RL_MEM_ZIPS 4

static struct {
    const uint8_t *data[RL_MEM_ZIPS];
    size_t         size[RL_MEM_ZIPS];
    int            count;                   /* 0: read from disk */
    bool           strict;
    int            missing;                 /* files the last load did not find */
    char           missing_names[512];      /* their names, space separated, for the UI */
} g_rl_mem_zip;

/* The first zip; rl_mem_zip_add gives the others (a split set's parent). */
static inline void rl_mem_zip_set(const uint8_t *data, size_t size, bool strict) {
    memset(&g_rl_mem_zip, 0, sizeof(g_rl_mem_zip));
    g_rl_mem_zip.strict = strict;
    g_rl_mem_zip.data[0] = data;
    g_rl_mem_zip.size[0] = size;
    g_rl_mem_zip.count   = 1;
}

/* One more zip after rl_mem_zip_set's. 0, or -1 when there is no room. */
static inline int rl_mem_zip_add(const uint8_t *data, size_t size) {
    if (g_rl_mem_zip.count < 1 || g_rl_mem_zip.count >= RL_MEM_ZIPS) return -1;
    g_rl_mem_zip.data[g_rl_mem_zip.count] = data;
    g_rl_mem_zip.size[g_rl_mem_zip.count] = size;
    g_rl_mem_zip.count++;
    return 0;
}

static inline void rl_mem_zip_clear(void) {
    memset(g_rl_mem_zip.data, 0, sizeof(g_rl_mem_zip.data));
    memset(g_rl_mem_zip.size, 0, sizeof(g_rl_mem_zip.size));
    g_rl_mem_zip.count = 0;
}

static inline void rl_mem_zip_note_missing(const char *filename) {
    g_rl_mem_zip.missing++;
    size_t used = strlen(g_rl_mem_zip.missing_names);
    size_t need = strlen(filename) + 2;
    if (used + need < sizeof(g_rl_mem_zip.missing_names)) {
        if (used) g_rl_mem_zip.missing_names[used++] = ' ';
        strcpy(g_rl_mem_zip.missing_names + used, filename);
    }
}

/* The file out of in-memory zip `z`, by CRC (by name too, when not strict). */
static inline uint8_t *zip_extract_mem_one(int z, const char *filename, size_t *out_size,
                                           uint32_t expected_crc) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, g_rl_mem_zip.data[z], g_rl_mem_zip.size[z], 0)) return NULL;

    void *data = NULL;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && !data; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        if ((uint32_t)st.m_crc32 != expected_crc) continue;
        data = mz_zip_reader_extract_to_heap(&zip, i, out_size, 0);
    }
    if (!data && !g_rl_mem_zip.strict)
        data = mz_zip_reader_extract_file_to_heap(&zip, filename, out_size, MZ_ZIP_FLAG_IGNORE_PATH);
    mz_zip_reader_end(&zip);
    return (uint8_t *)data;
}

static inline uint8_t *zip_extract_mem(const char *filename, size_t *out_size, uint32_t expected_crc) {
    uint8_t *data = NULL;
    for (int z = 0; z < g_rl_mem_zip.count && !data; z++)
        data = zip_extract_mem_one(z, filename, out_size, expected_crc);
    return data;
}

/* crc (optional) gets the file's CRC-32 as the zip's directory records it. That
 * is the CRC of the bytes returned: miniz checks every file it extracts against
 * it and fails the extraction on a mismatch (MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS
 * is not set), so computing it again from the data would only repeat that pass.
 * It used to be repeated, byte by byte, over the whole set: a quarter of the
 * time a desktop build took to load STF. */
static inline uint8_t *zip_extract(const char *zippath, const char *filename,
                                   size_t *out_size, uint32_t *crc) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zippath, 0)) return NULL;
    void *data = NULL;
    int i = mz_zip_reader_locate_file(&zip, filename, NULL, MZ_ZIP_FLAG_IGNORE_PATH);
    mz_zip_archive_file_stat st;
    if (i >= 0 && mz_zip_reader_file_stat(&zip, (mz_uint)i, &st)) {
        data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i, out_size, 0);
        if (data && crc) *crc = (uint32_t)st.m_crc32;
    }
    mz_zip_reader_end(&zip);
    return (uint8_t *)data;
}

/* The file in the zip whose CRC-32 is `crc`, whatever it is called. MAME finds
 * a set's files by CRC, so older dumps that name them differently still load
 * (Sega Rally's srallyc.zip from 2010 has "epr-17888.12" for MAME's
 * "epr-17888c.12", and the two copro_data ROMs' IC numbers swapped). */
static inline uint8_t *zip_extract_by_crc(const char *zippath, uint32_t crc, size_t *out_size) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zippath, 0)) return NULL;
    void *data = NULL;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && !data; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        if ((uint32_t)st.m_crc32 == crc) data = mz_zip_reader_extract_to_heap(&zip, i, out_size, 0);
    }
    mz_zip_reader_end(&zip);
    return (uint8_t *)data;
}

/* While > 0, a file not found is no error: a caller trying a chip that has a
 * stand-in (rom_recipe.h, an older revision's) says so itself if both miss. */
static int g_rl_quiet_missing;

/* Try the child zip first (e.g. sfight.zip), fall back to the parent
 * (e.g. schamp.zip) for files shared via MAME's clone mechanism.
 * Verifies CRC32 — mismatches log a warning but the buffer is still
 * returned, since hacked / mod sets are intentionally a use case. */
static inline uint8_t *zip_extract_from_set(const char *child_zip, const char *parent_zip,
                                            const char *filename, size_t *out_size,
                                            uint32_t expected_crc) {
    if (g_rl_mem_zip.count > 0) {
        uint8_t *mem = zip_extract_mem(filename, out_size, expected_crc);
        if (!mem && !g_rl_quiet_missing) {
            rl_mem_zip_note_missing(filename);
            LOG_ERROR("ROM not in the zip: %s (CRC %08X)", filename, expected_crc);
        }
        return mem;
    }
    uint8_t *data = NULL;
    uint32_t actual = 0;
    if (child_zip)  data = zip_extract(child_zip,  filename, out_size, &actual);
    if (!data && parent_zip) data = zip_extract(parent_zip, filename, out_size, &actual);
    if (!data && child_zip  && (data = zip_extract_by_crc(child_zip,  expected_crc, out_size))) actual = expected_crc;
    if (!data && parent_zip && (data = zip_extract_by_crc(parent_zip, expected_crc, out_size))) actual = expected_crc;
    if (!data) {
        if (!g_rl_quiet_missing) LOG_ERROR("ROM not found: %s", filename);
        return NULL;
    }

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
    /* The CPU board's coprocessor tables (Model 2 / 2A TGP: opr-14742a/43a). */
    uint8_t *tgp_tables;  size_t tgp_tables_size;
    /* An I/O board's own CPU program (the Model 1 I/O board's Z80: epr-14869c). */
    uint8_t *iocpu;       size_t iocpu_size;
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
    free(rs->tgp_tables);
    free(rs->iocpu);
    memset(rs, 0, sizeof(*rs));
}

/* ---- A romset on disk ---------------------------------------------------- */

/* A ROM set can also be a directory of straight images, one file a region,
 * each byte for byte what load_fn leaves in memory: no zip to inflate, no
 * interleave or byte swap, no CRC pass. `--export-roms DIR` writes one from a
 * zip. The directory's name picks the profile, as the zip's basename does. A
 * region whose file is missing stays NULL, as it does for a set without it. */
#define ROMSET_FILES 9
static const char *const k_romset_files[ROMSET_FILES] = {
    "maincpu.bin", "main_data.bin", "copro_data.bin", "polygons.bin",
    "textures.bin", "audiocpu.bin", "samples.bin", "tgp_tables.bin", "iocpu.bin",
};

static inline uint8_t **romset_region(romset_t *rs, int i, size_t **size) {
    uint8_t **r[ROMSET_FILES] = { &rs->maincpu, &rs->main_data, &rs->copro_data, &rs->polygons,
                                  &rs->textures, &rs->audiocpu, &rs->samples, &rs->tgp_tables,
                                  &rs->iocpu };
    size_t *z[ROMSET_FILES] = { &rs->maincpu_size, &rs->main_data_size, &rs->copro_data_size,
                                &rs->polygons_size, &rs->textures_size, &rs->audiocpu_size,
                                &rs->samples_size, &rs->tgp_tables_size,
                                &rs->iocpu_size };
    *size = z[i];
    return r[i];
}

static inline bool romset_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static inline int romset_load_dir(romset_t *rs, const char *dir) {
    romset_free(rs);
    for (int i = 0; i < ROMSET_FILES; i++) {
        char path[1024];
        size_t *size;
        uint8_t **region = romset_region(rs, i, &size);
        snprintf(path, sizeof path, "%s/%s", dir, k_romset_files[i]);
        *region = file_load(path, size);
    }
    if (!rs->maincpu) { LOG_ERROR("ROM directory %s has no %s", dir, k_romset_files[0]); romset_free(rs); return -1; }
    rs->loaded = true;
    LOG_INFO("ROM set loaded from the directory %s", dir);
    return 0;
}

static inline int romset_save_dir(const romset_t *rs, const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0777);
#endif
    for (int i = 0; i < ROMSET_FILES; i++) {
        char path[1024];
        size_t *size;
        uint8_t **region = romset_region((romset_t *)rs, i, &size);
        if (!*region) continue;
        snprintf(path, sizeof path, "%s/%s", dir, k_romset_files[i]);
        FILE *f = fopen(path, "wb");
        bool ok = f && fwrite(*region, 1, *size, f) == *size;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) { LOG_ERROR("cannot write %s", path); return -1; }
    }
    return 0;
}

/* Load a ROM set from a zip (the profile's load_fn, with its parent) or from
 * a directory of region images (romset_load_dir). */
static inline int romset_load(romset_t *rs, int (*load_fn)(romset_t *, const char *, const char *),
                              const char *path, const char *parent) {
    if (romset_is_dir(path)) return romset_load_dir(rs, path);
    return load_fn(rs, path, parent);
}

#endif /* ROM_LOADER_H */
