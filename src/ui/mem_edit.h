/*
 * mem_edit.h — C facade over vendor/imgui_club's MemoryEditor widget.
 *
 * imgui_club is a C++ header written against the ImGui C++ API; the rest of
 * src/ is C11 against cimgui. mem_edit.cpp is the one translation unit that
 * bridges the two — see the conventions note there.
 *
 * The widget never gets a pointer to emulator memory. An address space here
 * is a region table with MMIO callbacks, not a flat buffer, so every byte the
 * editor shows or edits goes through the caller's read/write functions, which
 * receive *absolute* addresses.
 */
#ifndef MEM_EDIT_H
#define MEM_EDIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mem_edit mem_edit_t;

typedef uint8_t (*mem_edit_read_fn)(void *user, uint32_t addr);
typedef void    (*mem_edit_write_fn)(void *user, uint32_t addr, uint8_t val);

/* One editor per window, created on first draw and kept for the process.
 * `addr_digits` fixes the address column width (8 for the i960's 32-bit
 * space, 6 for the 68K's 24-bit one) so the layout does not jump when the
 * view moves between regions with different top addresses. */
mem_edit_t *mem_edit_create(int addr_digits);

void mem_edit_set_bus(mem_edit_t *me, mem_edit_read_fn rd, mem_edit_write_fn wr, void *user);

/* Draws the hex/ASCII grid, options line and data preview into the current
 * window, filling the space left below whatever the caller drew above it.
 * [base, base+size) is the address window on show. */
void mem_edit_draw(mem_edit_t *me, uint32_t base, uint32_t size);

/* Scroll to `addr` and select it on the next draw. Ignored if that draw's
 * window does not cover it, so callers may switch region and jump in either
 * order. */
void mem_edit_goto(mem_edit_t *me, uint32_t addr);

/* Address of the byte the user last clicked, if any. */
bool mem_edit_selected(mem_edit_t *me, uint32_t *out_addr);

#ifdef __cplusplus
}
#endif

#endif /* MEM_EDIT_H */
