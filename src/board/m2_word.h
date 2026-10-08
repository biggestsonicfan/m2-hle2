/* m2_word.h -- aligned word loads and stores, shared by memory.h and the COP. */
#ifndef M2_WORD_H
#define M2_WORD_H

#include <stdint.h>
#include <string.h>

/* A word at a 4-aligned address in a byte buffer (bufferram, a region's
 * backing). memcpy says the same, but for a pointer of unknown alignment the
 * SH4's GCC calls the library for it: 4-byte copies were ~2% of a Dreamcast
 * frame. */
#if defined(__GNUC__) || defined(__clang__)
static inline uint32_t m2_ld32a(const void *p) {
    uint32_t v; __builtin_memcpy(&v, __builtin_assume_aligned(p, 4), 4); return v;
}
static inline void m2_st32a(void *p, uint32_t v) {
    __builtin_memcpy(__builtin_assume_aligned(p, 4), &v, 4);
}
#else
static inline uint32_t m2_ld32a(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void m2_st32a(void *p, uint32_t v) { memcpy(p, &v, 4); }
#endif

#endif /* M2_WORD_H */
