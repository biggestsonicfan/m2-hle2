/*
 * geo_rom.h — a loaded ROM set and its profile, as the model decoder takes them.
 */
#ifndef M2HLE_GEO_ROM_H
#define M2HLE_GEO_ROM_H

#include "game_profile.h"
#include "geo3d.h"
#include "rom_loader.h"

static inline geo3d_rom_t geo3d_rom_of(const romset_t *rs, const game_quirks_t *q) {
    return (geo3d_rom_t){
        .main_data = rs->main_data, .main_data_size = rs->main_data_size,
        .polygons  = rs->polygons,  .polygons_size  = rs->polygons_size,
        .materials = rs->textures,  .materials_size = rs->textures_size,
        .table_off = q->model_table_offset, .table_count = q->model_table_count,
        .mesh_ptr_subtract = q->mesh_ptr_subtract, .mesh_ptr_add = q->mesh_ptr_add,
    };
}

#endif /* M2HLE_GEO_ROM_H */
