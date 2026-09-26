/*
 * mdfn_scsp.h -- Mednafen's SCSP (the Saturn's, the same chip as Model 2's),
 * behind a C interface, for scsp_vs_mednafen.c. See README.md here.
 *
 * Addresses are the chip's own: sound RAM at 0x000000-0x07FFFF, the
 * registers at 0x100000 + the offset scsp.h takes (slots 0x000, common 0x400,
 * sound stack 0x600, DSP 0x700-0xEFF).
 */
#ifndef MDFN_SCSP_H
#define MDFN_SCSP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void     mdfn_scsp_reset(void);
uint16_t *mdfn_scsp_ram(void);               /* 256K words, host order */
void     mdfn_scsp_write(uint32_t addr, uint32_t v, int size);
uint32_t mdfn_scsp_read(uint32_t addr, int size);
int      mdfn_scsp_midi_in(uint8_t b);           /* 0: the 4-byte input FIFO was full, byte dropped */
void     mdfn_scsp_sample(int16_t *l, int16_t *r);
int      mdfn_scsp_irq_level(void);          /* the 68000's IPL, as last raised */
#ifdef __cplusplus
}
#endif
#endif
