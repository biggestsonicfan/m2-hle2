/*
 * dc_paged.h -- what the board's headers need to know of the Dreamcast's
 * software pager (dc_pager.h), put in front of every file by the Makefile
 * (-include), before memory.h and geo3d.h read the hooks.
 *
 * A paged region's data pointer is an address in the pager's window
 * (PG_VA_BASE on), which names its pages and is never dereferenced. The bus
 * pages one in on its slow path (MEM_HOST_PAGE_IN) and puts it in its direct
 * page tables; the 3D decoder and the profile's install read through
 * dc_rom_at, which gives the bytes' place in a frame.
 */
#ifndef DC_PAGED_H
#define DC_PAGED_H

#include <stdint.h>

#define PG_VA_BASE  0x10000000u
#define PG_VA_SPAN  0x08000000u          /* the windows (ROM and anonymous) fit in 128 MB */

struct memory_bus;
struct mem_region;
static uint8_t *pg_bus_in(struct memory_bus *bus, struct mem_region *r, uint32_t addr, int write);
static void pg_slots_reset(void);
static const uint8_t *dc_rom_at(const void *p, uint32_t n);

#define MEM_HOST_PAGED(data)                  ((uint32_t)(uintptr_t)(data) - PG_VA_BASE < PG_VA_SPAN)
#define MEM_HOST_PAGE_IN(bus, r, addr, write) pg_bus_in((bus), (r), (addr), (write))
#define MEM_HOST_PAGES_RESET()                pg_slots_reset()
#define MEM_HOST_AT(p, n)                     dc_rom_at((p), (n))
#define GEO3D_ROM(p, n)                       dc_rom_at((p), (n))

#endif /* DC_PAGED_H */
