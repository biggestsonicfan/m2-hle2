/*
 * sound.h — Model 2 sound block: MIDI stub + MC68000 CPU.
 *
 * Hardware (Model 2B/2C), verified against MAME model2_snd() and
 * schamp IDA disassembly (epr-19021):
 *
 *   68K address map:
 *     0x000000–0x07FFFF  sound / wave RAM (512 KB, shared with SCSP)
 *     0x100000–0x100FFF  SCSP registers (4 KB)
 *     0x400000–0x400001  sound control register
 *     0x600000–0x67FFFF  68K program ROM (512 KB, "audiocpu" region)
 *     0x800000–0x9FFFFF  sample ROM (2 MB, not yet loaded)
 *
 *   The 68K uses wave RAM (0x000000) for its own data structures:
 *     0x002800  SCSP channel RAM (per Explanation.txt)
 *     0x004000  track RAM
 *
 *   i960 ↔ 68K communication: the i960 writes to MIDI_BASE (0x9C0000 in
 *   i960 space); those writes route to the COMM buffer here, which the
 *   68K sees at M68K_SCSP_BASE (0x100000).
 *
 * What is implemented:
 *   - MC68000 CPU state + instruction executor (m68k_exec.h).
 *   - ROM buffer (512 KB).
 *   - Wave RAM buffer (512 KB) used by 68K and SCSP.
 *   - COMM buffer (4 KB) bridging i960 MIDI writes to 68K SCSP registers.
 *   - SCSP reads return 0; writes are silently absorbed until SCSP lands.
 *   - sound_step(n) runs n 68K instructions.
 *
 * What is not yet implemented:
 *   - SCSP tone generation / DSP / sample playback.
 *   - Interrupt delivery from SCSP to 68K.
 *   - Sample ROM loading (0x800000+).
 *   - Cycle-accurate scheduling between i960 and 68K.
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constants.h"
#include "log.h"
#include "memory.h"
#include "m68k.h"
#include "m68k_exec.h"
#include "scsp_hle.h"

/* ---- state --------------------------------------------------------------- */

typedef struct {
    /* i960-side accounting (used by the UI) */
    uint64_t write_count;
    uint64_t read_count;
    bool     log_writes;

    /* Diagnostic: log of MIDI bytes the i960 sent to MIDI_BASE, with the i960 IP
     * that issued each write (so we can identify the sound-send routine). */
    uint8_t  midi_log[64];
    uint32_t midi_log_ip[64];
    uint32_t midi_log_n;

    /* MIDI byte FIFO (i960 → 68K). The i960 sends commands in bursts on its own
     * slice; comm[0x405] holds only ONE byte and Int3 delivers one byte per
     * interrupt, so without a queue all but the last byte of a burst are lost.
     * sound_midi_write_cb enqueues; sound_step delivers one byte per Int3 as the
     * 68K becomes ready (IPL < 3). */
    uint8_t  midi_fifo[256];
    int      midi_fifo_head;   /* write index (producer: i960) */
    int      midi_fifo_tail;   /* read index  (consumer: 68K)  */
    uint32_t midi_fifo_drops;  /* diag: bytes dropped on FIFO full */

    /* Diagnostic: full 0x20-byte slot register block of the most recent key-on,
     * plus the wave-RAM bytes at its sample start address (to check whether the
     * PCM data is actually present in wave RAM). */
    uint8_t  dbg_slot_regs[32];
    uint8_t  dbg_slot_idx;
    uint32_t dbg_sa;
    uint8_t  dbg_wave_at_sa[16];

    /* 68K CPU */
    m68k_state_t m68k;
    bool         rom_loaded;

    /* 68K address-space buffers */
    uint8_t wave[M68K_WAVE_SIZE];  /* sound / wave RAM (512 KB) at 0x000000 */
    uint8_t comm[M68K_SCSP_SIZE];  /* SCSP register window (4 KB) at 0x100000 */
    uint8_t rom [M68K_ROM_SIZE ];  /* program ROM (512 KB) at 0x600000      */

    /* Sample ROM window: 68K sees this at 0x800000–0x9FFFFF (2 MB).
     * Loaded from romset.samples (8 MB total; first 2 MB cover the window). */
    const uint8_t *samples;        /* pointer into romset.samples — not owned */
    uint32_t       samples_size;   /* bytes valid at samples ptr (≤ M68K_SAMPLE_SIZE) */
    uint32_t       sample_rom_reads; /* diag: # of 68K reads of the sample ROM window */
    /* diag: DoSoundCmd (0x6034B8) hits + the two arg bytes it reads */
    uint32_t dbg_dsc_count; uint8_t dbg_dsc_b0, dbg_dsc_b1;
    /* diag: song-load path counters */
    uint32_t dbg_loadsong;   /* LoadSong 0x603518 */
    uint32_t dbg_sc07;       /* SC07_InitTrkIns 0x603B12 (song-load handler entry) */
    uint32_t dbg_sc07_post;  /* SC07 past the SC05 call → track setup 0x603BB4 */
    uint32_t dbg_sc05;       /* SC05_LoadIns 0x603A52 (instrument copy) */
    uint32_t dbg_dsc7;       /* DoSoundCmd calls with data0&0x7F == 7 */
    uint8_t  dbg_sc07_d0;    /* d0 (song/instrument index) at SC07 entry */
    uint32_t dbg_sc07_stk[6];/* top 6 stack longwords at first SC07 entry (call chain) */
    int      dbg_sc07_stk_done;
    uint32_t dbg_dsc_song, dbg_dsc_fade, dbg_dsc_zero; /* DoSoundCmd data0 buckets */
    uint8_t  dbg_dsc_song_b0, dbg_dsc_song_b1;          /* last song command bytes */
    uint8_t  dbg_dsc_zero_b1;                           /* data1 of last data0==0 command */
    uint32_t dbg_enq[11];    /* hits per ring enqueue-commit site */
    uint32_t dbg_c3, dbg_c4; /* SndCmd00 sub[3] / sub[4] handler entries */
    uint32_t dbg_d1hist[16]; /* data1 histogram for data0==0 commands */
    int32_t  dbg_cmd_leak;   /* SP delta across one DoSoundCmd (expect +4 if balanced) */
    int32_t  dbg_chain_leak; /* SP delta between consecutive voice-chain tops (expect 0) */
    uint32_t dbg_cmd1_stk[6]; int dbg_cmd1_done; /* stack at first cmd1 (seed hunt) */
    uint32_t scsp_keyed;     /* bitmask of slots currently keyed on (KYONB committed) */
    uint16_t dbg_scsprd_off[32]; uint8_t dbg_scsprd_val[32]; uint32_t dbg_scsprd_pc[32]; uint32_t dbg_scsprd_cnt[32]; int dbg_scsprd_n; /* distinct SCSP ctrl-region (off>=0x400) reads */
    uint32_t dbg_ctrl_pc[64]; uint16_t dbg_ctrl_si[64]; int dbg_ctrl_pos; /* slot+0x00 write log (pc, slot<<8|ctrl) */
    uint32_t dbg_crash_pc, dbg_crash_target, dbg_crash_sp; int dbg_crash_done;
    uint32_t dbg_crash_stk[8]; /* stack around SP at crash (sp-16 .. sp+12) */
    uint32_t dbg_pcr[48];    /* circular PC ring */
    int      dbg_pcr_pos;
    uint32_t dbg_pcr_snap[128];/* forward (pc) trace from first DoSoundCmd */
    uint32_t dbg_pcr_sp[128];  /* SP at each trace step */
    int      dbg_pcr_snapped;
} sound_state_t;

static sound_state_t g_sound = {0};

/* ---- big-endian buffer helpers ------------------------------------------ */

static inline uint32_t buf_read(const uint8_t *buf, uint32_t off, int sz) {
    if (sz == 1) return buf[off];
    if (sz == 2) return ((uint32_t)buf[off] << 8) | buf[off+1];
    return ((uint32_t)buf[off]   << 24) | ((uint32_t)buf[off+1] << 16) |
           ((uint32_t)buf[off+2] <<  8) |  (uint32_t)buf[off+3];
}
static inline void buf_write(uint8_t *buf, uint32_t off, uint32_t val, int sz) {
    if (sz == 1) { buf[off] = (uint8_t)val; return; }
    if (sz == 2) { buf[off] = (uint8_t)(val>>8); buf[off+1] = (uint8_t)val; return; }
    buf[off]   = (uint8_t)(val>>24); buf[off+1] = (uint8_t)(val>>16);
    buf[off+2] = (uint8_t)(val>>8);  buf[off+3] = (uint8_t)val;
}

/* ---- 68K bus callbacks --------------------------------------------------- */

static uint32_t sound_m68k_read(void *ctx, uint32_t addr, int sz) {
    sound_state_t *ss = (sound_state_t *)ctx;
    addr &= 0xFFFFFFu;

    /* Wave / sound RAM: 0x000000–0x07FFFF */
    if (addr < M68K_WAVE_SIZE) {
        if (addr + (uint32_t)sz <= M68K_WAVE_SIZE) return buf_read(ss->wave, addr, sz);
        return 0;
    }
    /* SCSP registers: 0x100000–0x100FFF */
    if (addr >= M68K_SCSP_BASE && addr < M68K_SCSP_BASE + M68K_SCSP_SIZE) {
        uint32_t off = addr - M68K_SCSP_BASE;
        {   /* DIAG: log distinct SCSP reads. Slot regs (off<0x400) keyed by the
             * in-slot offset (off&0x1F)|0x8000 so all 32 slots fold together. */
            uint16_t key = (off >= 0x400u) ? (uint16_t)off
                                           : (uint16_t)(0x8000u | (off & 0x1Fu));
            int found = -1;
            for (int i = 0; i < ss->dbg_scsprd_n; i++)
                if (ss->dbg_scsprd_off[i] == key) { found = i; break; }
            if (found < 0 && ss->dbg_scsprd_n < 32) {
                found = ss->dbg_scsprd_n++;
                ss->dbg_scsprd_off[found] = key;
                ss->dbg_scsprd_pc[found]  = ss->m68k.cpu.pc;
            }
            if (found >= 0) {
                ss->dbg_scsprd_val[found] = ss->comm[off];
                ss->dbg_scsprd_cnt[found]++;
            }
        }
        if (off + (uint32_t)sz <= M68K_SCSP_SIZE) return buf_read(ss->comm, off, sz);
        return 0;
    }
    /* Sound control register: 0x400000–0x400001 */
    if (addr >= M68K_SNDCTL_BASE && addr < M68K_SNDCTL_BASE + M68K_SNDCTL_SIZE) {
        return 0; /* stub — returns 0 until SCSP is active */
    }
    /* Program ROM: 0x600000–0x67FFFF */
    if (addr >= M68K_ROM_BASE && addr < M68K_ROM_BASE + M68K_ROM_SIZE) {
        if (!ss->rom_loaded) return 0;
        uint32_t off = addr - M68K_ROM_BASE;
        if (off + (uint32_t)sz <= M68K_ROM_SIZE) return buf_read(ss->rom, off, sz);
        return 0;
    }
    /* Sample ROM: 0x800000–0x9FFFFF (2 MB window) */
    if (addr >= M68K_SAMPLE_BASE && addr < M68K_SAMPLE_BASE + M68K_SAMPLE_SIZE) {
        if (!ss->samples) return 0;
        ss->sample_rom_reads++;
        uint32_t off = addr - M68K_SAMPLE_BASE;
        if (off + (uint32_t)sz <= ss->samples_size) return buf_read(ss->samples, off, sz);
        return 0;
    }
    return 0;
}

/* Push a KEYON for one slot, decoding its parameters from comm[]. */
static inline void sound_scsp_keyon(sound_state_t *ss, uint32_t slot_idx) {
    uint32_t off   = slot_idx * 0x20u;
    uint8_t  w0_lo = ss->comm[off + 0x01];
    bool     pcm8b = (w0_lo >> 4) & 1;
    uint32_t sa    = ((uint32_t)(w0_lo & 0x0Fu) << 16)
                   | ((uint32_t)ss->comm[off + 0x02] << 8) | ss->comm[off + 0x03];
    uint32_t lsa   = ((uint32_t)ss->comm[off + 0x04] << 8) | ss->comm[off + 0x05];
    uint32_t lea   = ((uint32_t)ss->comm[off + 0x06] << 8) | ss->comm[off + 0x07];
    uint8_t  loopm = (w0_lo >> 5) & 3u;
    uint32_t pw    = ((uint32_t)ss->comm[off + 0x10] << 8) | ss->comm[off + 0x11];
    uint16_t oct   = (uint16_t)((pw >> 11) & 0xF);
    uint16_t fns   = (uint16_t)(pw & 0x3FF);
    uint8_t  tl    = ss->comm[off + 0x0D];
    uint32_t dw    = ((uint32_t)ss->comm[off + 0x16] << 8) | ss->comm[off + 0x17];
    uint8_t  disdl = (uint8_t)((dw >> 13) & 7u);
    uint8_t  dipan = (uint8_t)((dw >> 8) & 0x1Fu);
    if (sa != 0xFFFFFu) {  /* diag: snapshot the slot regs of a real note */
        g_sound.dbg_slot_idx = (uint8_t)slot_idx;
        for (int _b = 0; _b < 32; _b++) g_sound.dbg_slot_regs[_b] = ss->comm[off + (uint32_t)_b];
        g_sound.dbg_sa = sa;
        for (int _w = 0; _w < 16; _w++)
            g_sound.dbg_wave_at_sa[_w] = (sa + (uint32_t)_w < M68K_WAVE_SIZE) ? ss->wave[sa + (uint32_t)_w] : 0;
    }
    /* SA = all-ones is an uninitialised slot, not a real note — skip. */
    if (g_scsp.loaded && sa != 0xFFFFFu) {
        g_scsp.keyon_count++;
        scsp_ring_push(&g_scsp.ring, (scsp_evt_t){
            .type = SCSP_EVT_KEYON, .slot = (uint8_t)slot_idx,
            .pcm8b = (uint8_t)pcm8b, .loop = loopm,
            .sa = sa, .lsa = lsa, .lea = lea, .oct = oct, .fns = fns,
            .tl = tl, .disdl = disdl, .dipan = dipan });
    }
}

static void sound_m68k_write(void *ctx, uint32_t addr, uint32_t val, int sz) {
    sound_state_t *ss = (sound_state_t *)ctx;
    addr &= 0xFFFFFFu;

    /* Wave / sound RAM */
    if (addr < M68K_WAVE_SIZE) {
        if (addr + (uint32_t)sz <= M68K_WAVE_SIZE) buf_write(ss->wave, addr, val, sz);
        return;
    }
    /* SCSP registers */
    if (addr >= M68K_SCSP_BASE && addr < M68K_SCSP_BASE + M68K_SCSP_SIZE) {
        uint32_t off = addr - M68K_SCSP_BASE;
        if (off + (uint32_t)sz <= M68K_SCSP_SIZE) buf_write(ss->comm, off, val, sz);

        /* SCIRE (reg 0x422): writing a bit clears the matching SCIPD (0x420)
         * interrupt-pending bit (SCSP hardware, MAME scsp.cpp case 0x22).  The
         * Int2_Timer ISR clears bit 7 (Timer B path) / bit 8 (Timer C path)
         * here; without it SCIPD bits stick and the bit7/bit8 dispatch breaks. */
        if (off == 0x422u && sz == 2) {
            uint32_t scipd = ((uint32_t)ss->comm[0x420] << 8) | ss->comm[0x421];
            scipd &= ~(val & 0xFFFFu);
            ss->comm[0x420] = (uint8_t)(scipd >> 8);
            ss->comm[0x421] = (uint8_t)scipd;
        }
        static uint32_t s_scsp_writes = 0;
        if (++s_scsp_writes <= 4)
            LOG_INFO("sound: SCSP write[%u] off=0x%03X val=0x%02X sz=%d pc=0x%06X",
                     s_scsp_writes, off, val, sz, ss->m68k.cpu.pc);

        /* Intercept SCSP slot control byte (slot*0x20 + 0x00) in slot register
         * area [0x100000..0x1003FF].  Each slot is 0x20 bytes.
         *
         * Key-on:  KYONB(bit3)=1 and KYONEX(bit4)=1 → both set
         * Key-off: KYONEX(bit4)=1, KYONB(bit3)=0
         *
         * On key-on, decode SA/LSA/LEA/PCM8B from the slot's own registers
         * (already written to ss->comm[] by the 68K before the key-on byte):
         *   comm[off+0x01]       = low byte of word 0: PCM8B(bit4), SA[19:16]
         *   comm[off+0x02..0x03] = SA[15:0]  (big-endian word)
         *   comm[off+0x04..0x05] = LSA       (big-endian word, sample index)
         *   comm[off+0x06..0x07] = LEA       (big-endian word, sample index)
         */
        /* SCSP key-on/off uses a GLOBAL strobe: KYONEX (slot+0x00 bit 4) commits
         * every slot's KYONB (bit 3) at once.  So on any write to a slot's word-0
         * high byte that has KYONEX set, scan ALL 32 slots and reconcile against
         * the keyed-on bitmask: KYONB=1 & not keyed → key-on; KYONB=0 & keyed →
         * key-off.  (Per-slot detection missed key-offs done by clearing KYONB on
         * one slot then strobing KYONEX on another — those notes looped forever.) */
        if (off < 0x400u && (off & 0x1Fu) == 0u) {
            /* DIAG: log every slot-control write (PC + slot<<8|ctrl) */
            g_sound.dbg_ctrl_pc[g_sound.dbg_ctrl_pos] = ss->m68k.cpu.pc;
            g_sound.dbg_ctrl_si[g_sound.dbg_ctrl_pos] =
                (uint16_t)(((off / 0x20u) << 8) | ss->comm[off + 0x00u]);
            g_sound.dbg_ctrl_pos = (g_sound.dbg_ctrl_pos + 1) & 63;
        }
        if (off < 0x400u && (off & 0x1Fu) == 0u && (ss->comm[off + 0x00u] & 0x10u)) {
            for (uint32_t s = 0; s < 32u; s++) {
                uint32_t bit   = 1u << s;
                int      kyonb = (ss->comm[s * 0x20u + 0x00u] >> 3) & 1;
                if (kyonb && !(ss->scsp_keyed & bit)) {
                    sound_scsp_keyon(ss, s);
                    ss->scsp_keyed |= bit;
                } else if (!kyonb && (ss->scsp_keyed & bit)) {
                    if (g_scsp.loaded) {
                        g_scsp.keyoff_count++;
                        scsp_ring_push(&g_scsp.ring, (scsp_evt_t){
                            .type = SCSP_EVT_KEYOFF, .slot = (uint8_t)s });
                    }
                    ss->scsp_keyed &= ~bit;
                }
            }
            /* SCSP auto-clears KYONEX on the written slot once the strobe is
             * committed (MAME scsp.cpp:701 `data[0] &= ~0x1000`). The driver
             * reads slot+0x00 back (e.g. @0x602276) and branches on it; leaving
             * KYONEX set diverged the voice logic from hardware. */
            ss->comm[off + 0x00u] &= (uint8_t)~0x10u;
        }
        return;
    }
    /* Sound control register — absorb silently */
    if (addr >= M68K_SNDCTL_BASE && addr < M68K_SNDCTL_BASE + M68K_SNDCTL_SIZE) {
        return;
    }
    /* ROM writes are ignored */
}

/* ---- i960-side MIDI MMIO callbacks -------------------------------------- */

static uint32_t sound_midi_read_cb(mem_region_t *r, uint32_t addr, int size) {
    (void)r;
    g_sound.read_count++;
    uint32_t off = addr - MIDI_BASE;
    if (off + (uint32_t)size <= M68K_SCSP_SIZE)
        return buf_read(g_sound.comm, off, size);
    return 0;
}

static void sound_midi_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    (void)r; (void)size;
    /* Model 2B i960→68K serial (MAME model2.cpp): 0x9C0000 = UART DATA (the MIDI
     * sound bytes), 0x9C0004 = i8251 UART CONTROL (boot init: reset/mode/command
     * words like 40 4E 37). Only DATA writes are MIDI bytes — routing the control
     * bytes into the 68K's MIDI input pollutes the command stream and misframes
     * the driver. Absorb anything that isn't the data register. */
    if ((addr - MIDI_BASE) != 0) return;
    g_sound.write_count++;
    if (g_sound.midi_log_n < 64) {
        g_sound.midi_log[g_sound.midi_log_n]    = (uint8_t)val;
        g_sound.midi_log_ip[g_sound.midi_log_n] = g_mem_last_write_ip;
        g_sound.midi_log_n++;
    }
    if (g_sound.log_writes)
        LOG_INFO("SOUND write @ 0x%08X sz=%d val=0x%08X", addr, size, val);
    /* The i960 always uses a 32-bit store; the MIDI byte is in the LSB.
     * Enqueue it; sound_step paces delivery to comm[0x405] + Int3 one byte at a
     * time as the 68K consumes them (a burst here would otherwise overwrite
     * comm[0x405] and lose every byte but the last). */
    int next = (g_sound.midi_fifo_head + 1) & 0xFF;
    if (next != g_sound.midi_fifo_tail) {
        g_sound.midi_fifo[g_sound.midi_fifo_head] = (uint8_t)val;
        g_sound.midi_fifo_head = next;
    } else {
        g_sound.midi_fifo_drops++;   /* FIFO full — should not happen in practice */
    }
}

/* ---- lifecycle ----------------------------------------------------------- */

static inline void sound_reset(void) {
    g_sound.write_count = 0;
    g_sound.read_count  = 0;
    g_sound.midi_fifo_head = g_sound.midi_fifo_tail = 0;
    g_sound.midi_fifo_drops = 0;
    g_sound.scsp_keyed = 0;
    memset(g_sound.wave, 0, sizeof(g_sound.wave));
    memset(g_sound.comm, 0, sizeof(g_sound.comm));
    m68k_reset(&g_sound.m68k);
    g_sound.m68k.read_cb  = sound_m68k_read;
    g_sound.m68k.write_cb = sound_m68k_write;
    g_sound.m68k.mem_ctx  = &g_sound;
    /* leave log_writes as-is */
}

/*
 * Load the 68K sound program ROM from raw bytes.  bytes must be in native
 * ROM order (big-endian; straight from the MAME zip).  size must be
 * ≤ M68K_ROM_SIZE (512 KB).
 *
 * The reset vectors (SSP at ROM[0], PC at ROM[4]) are read directly from
 * the ROM bytes rather than from the 68K address space, because the ROM is
 * mapped at 0x600000 and the CPU reads vectors from 0x000000 (wave RAM)
 * which is zeroed at boot time.  The game ROM's vector table stores
 * absolute addresses like 0x60xxxx, so this correctly sets the entry point.
 */
static inline void sound_load_rom(const uint8_t *bytes, uint32_t size) {
    if (size > M68K_ROM_SIZE) {
        LOG_WARN("sound: ROM size %u > %u — truncating", size, M68K_ROM_SIZE);
        size = M68K_ROM_SIZE;
    }
    memcpy(g_sound.rom, bytes, size);
    if (size < M68K_ROM_SIZE)
        memset(g_sound.rom + size, 0xFF, M68K_ROM_SIZE - size);
    g_sound.rom_loaded = true;

    /* Bootstrap from reset vectors stored at ROM offset 0 (big-endian). */
    uint32_t ssp = buf_read(g_sound.rom, 0, 4);
    uint32_t pc  = buf_read(g_sound.rom, 4, 4);
    g_sound.m68k.cpu.ssp  = ssp;
    g_sound.m68k.cpu.a[7] = ssp;
    g_sound.m68k.cpu.pc   = pc;
    LOG_INFO("sound: 68K ROM loaded (%u bytes), SSP=0x%08X PC=0x%08X", size, ssp, pc);
}

/*
 * Point the 68K's sample ROM window at an external buffer (romset.samples).
 * The buffer is not copied — caller must keep it alive for the session.
 * size is clamped to M68K_SAMPLE_SIZE (2 MB); the full 8 MB romset may be
 * larger, but the 68K window only covers the first 2 MB.
 */
static inline void sound_load_samples(const uint8_t *bytes, uint32_t size) {
    if (size > M68K_SAMPLE_SIZE) size = M68K_SAMPLE_SIZE;
    g_sound.samples      = bytes;
    g_sound.samples_size = size;
    LOG_INFO("sound: sample ROM mapped (%u bytes at 0x%08X)", size, M68K_SAMPLE_BASE);
}

/* Hook the MIDI region callbacks.  Call after mem_init(). */
static inline void sound_attach(memory_bus_t *bus) {
    for (int i = 0; i < bus->region_count; i++) {
        mem_region_t *r = &bus->regions[i];
        if (r->base == MIDI_BASE) {
            r->read_cb  = sound_midi_read_cb;
            r->write_cb = sound_midi_write_cb;
            LOG_INFO("sound: MIDI callbacks attached");
            return;
        }
    }
    LOG_WARN("sound: MIDI region not found — sound inactive");
}

/*
 * Timer B fires when SCSP register 0x421 bit 7 is set; that routes the ISR
 * to the BGM track (a2=$2000(a6)).  We set this flag before every fire so
 * the 68K always runs the main music path.
 *
 * Interval ≈ SCSP clock 11.289 MHz / (50 counts × 64 prescaler) ≈ 3528 Hz.
 * At ~225 000 68K steps/slice × 60 Hz = 13.5 M steps/s:
 *   interval = 13 500 000 / 3528 ≈ 3825 steps.
 */
#define SOUND_TIMER_INTERVAL 3825u

static uint32_t g_sound_timer_cnt = 0;     /* Timer B (bit 7) — track @A6+0x2000 */
static uint32_t g_sound_timer_cnt_c = 0;   /* Timer C (bit 8) — 6 tracks @A6+0x2010.. */
static uint32_t g_sound_timer_cnt_a = 0;   /* Timer A (level-1 IRQ) — voice duration/key-off */
static uint32_t g_sound_noteon = 0;        /* DIAG: MidEvt_NoteOn  (0x601AF8) calls */
static uint32_t g_sound_noteoff = 0;       /* DIAG: MidEvt_NoteOff (0x6027F0) calls */
static int      g_sound_int2_disable = 0;  /* DIAG: Int2 injection (0=on). Not the storm cause. */
static uint32_t g_sound_dsc_sp_in = 0;     /* DIAG: SP at DoSoundCmd entry, for leak measure */
static uint32_t g_sound_chain_sp = 0;      /* DIAG: SP at last voice-chain top */
static int      g_sound_fwd_active = 0;     /* DIAG: forward-trace armed */

/*
 * Run up to n 68K instructions.  Returns instructions actually executed.
 * No-op if ROM is not loaded or the CPU is halted/stopped.
 */
static uint32_t g_sound_step_total = 0;

static inline int sound_step(int n) {
    if (!g_sound.rom_loaded) return 0;
    if (g_sound.m68k.cpu.halted) {
        static bool s_halt_logged = false;
        if (!s_halt_logged) {
            s_halt_logged = true;
            LOG_WARN("sound: 68K halted at PC=0x%06X after %u total steps",
                     g_sound.m68k.cpu.pc, g_sound_step_total);
        }
        return 0;
    }
    /* SCSP Timer B → 68K level-2 IRQ (Int2_Timer) = the BGM sequencer clock.
     * Rate is computed from the timer register the 68K programmed (SCSP clock
     * 22.5792 MHz, MAME scsp.cpp):
     *   hz = 22579200 / (512 * (1<<prescaler) * (255 - reload))
     * Timer B reg = comm[0x41A] (high: prescaler bits[2:0]) / comm[0x41B] (reload).
     * We fire `int2_per_slice` interrupts per sound_step call, paced to the 60 Hz
     * slice clock so the tempo is correct in real time regardless of 68K step rate. */
    uint32_t presc  = g_sound.comm[0x41A] & 7u;
    uint32_t reload = g_sound.comm[0x41B];
    int int2_per_slice = 8;                       /* fallback until timer programmed */
    if (reload < 255u) {
        double hz = 22579200.0 / (512.0 * (double)(1u << presc) * (double)(255u - reload));
        int2_per_slice = (int)(hz / 60.0 + 0.5);  /* EMU pacing = 60 slices/sec */
        if (int2_per_slice < 1)   int2_per_slice = 1;
        if (int2_per_slice > 400) int2_per_slice = 400;
    }
    int int2_interval = n / int2_per_slice;
    if (int2_interval < 1) int2_interval = 1;

    int done = 0;
    while (done < n) {
        /* Paced MIDI delivery: feed one queued byte per Int3, only when the 68K
         * can accept it (IPL < 3, i.e. the previous Int3_MidiIn has returned).
         * comm[0x405] is read early in the handler, so it is safe to overwrite
         * once IPL has dropped again. */
        if (g_sound.midi_fifo_tail != g_sound.midi_fifo_head) {
            int ipl = (g_sound.m68k.cpu.sr >> 8) & 7;
            if (ipl < 3) {
                g_sound.comm[0x405] = g_sound.midi_fifo[g_sound.midi_fifo_tail];
                if (m68k_interrupt(&g_sound.m68k, 3)) {
                    g_sound.midi_fifo_tail = (g_sound.midi_fifo_tail + 1) & 0xFF;
                }
            }
        }
        uint32_t pre_pc = g_sound.m68k.cpu.pc;
        if (!m68k_step(&g_sound.m68k)) break;
        done++;
        uint32_t pc = g_sound.m68k.cpu.pc;
        /* Crash detector: ROM (>=0x600000) executing an instruction that jumps
         * into RAM (<0x100000) = a wild jump (corrupt rts/jump). Capture once. */
        if (!g_sound.dbg_crash_done && pre_pc >= 0x600000u && pc < 0x100000u) {
            g_sound.dbg_crash_pc     = pre_pc;
            g_sound.dbg_crash_target = pc;
            g_sound.dbg_crash_sp     = g_sound.m68k.cpu.a[7];
            uint32_t sp0 = g_sound.m68k.cpu.a[7];
            for (int _i = 0; _i < 8; _i++)
                g_sound.dbg_crash_stk[_i] = g_sound.m68k.read_cb(g_sound.m68k.mem_ctx, (sp0 - 16u) + (uint32_t)_i*4u, 4);
            g_sound.dbg_pcr_snapped = 1;   /* freeze the circular trace at the crash */
            g_sound.dbg_crash_done   = 1;
        }
        /* Circular (pc,sp) ring, frozen at the crash so dump shows the lead-up. */
        if (!g_sound.dbg_pcr_snapped) {
            g_sound.dbg_pcr_snap[g_sound.dbg_pcr_pos] = pc;
            g_sound.dbg_pcr_sp[g_sound.dbg_pcr_pos]   = g_sound.m68k.cpu.a[7];
            g_sound.dbg_pcr_pos = (g_sound.dbg_pcr_pos + 1) % 128;
        }
        if (pc == 0x601A90u) {   /* dequeue: just after DoSoundCmd returned */
            g_sound.dbg_cmd_leak = (int32_t)(g_sound.m68k.cpu.a[7] - g_sound_dsc_sp_in);
        }
        if (pc == 0x6034B8u) {   /* DoSoundCmd entry */
            g_sound_dsc_sp_in = g_sound.m68k.cpu.a[7];
            uint32_t a3 = g_sound.m68k.cpu.a[3];
            g_sound.dbg_dsc_count++;
            g_sound.dbg_dsc_b0 = (uint8_t)g_sound.m68k.read_cb(g_sound.m68k.mem_ctx, a3,     1);
            g_sound.dbg_dsc_b1 = (uint8_t)g_sound.m68k.read_cb(g_sound.m68k.mem_ctx, a3 + 1u, 1);
            if ((g_sound.dbg_dsc_b0 & 0x7Fu) == 7u) g_sound.dbg_dsc7++;
            uint8_t b = g_sound.dbg_dsc_b0 & 0x7Fu;
            if (b >= 0x10u && b < 0x70u) {        /* song-class (2-level) command */
                g_sound.dbg_dsc_song++;
                g_sound.dbg_dsc_song_b0 = g_sound.dbg_dsc_b0;
                g_sound.dbg_dsc_song_b1 = g_sound.dbg_dsc_b1;
            } else if (b == 1u) g_sound.dbg_dsc_fade++;
            else if (b == 0u) {
                g_sound.dbg_dsc_zero++; g_sound.dbg_dsc_zero_b1 = g_sound.dbg_dsc_b1;
                if (g_sound.dbg_dsc_b1 < 16) g_sound.dbg_d1hist[g_sound.dbg_dsc_b1]++;
            }
        }
        else if (pc == 0x6041EAu) {   /* voice-chain loop top */
            g_sound.dbg_chain_leak = (int32_t)(g_sound.m68k.cpu.a[7] - g_sound_chain_sp);
            g_sound_chain_sp = g_sound.m68k.cpu.a[7];
        }
        else if (pc == 0x603518u) g_sound.dbg_loadsong++;
        if (pc == 0x6036A2u && !g_sound.dbg_cmd1_done) {  /* FIRST cmd1 entry: dump stack */
            uint32_t sp = g_sound.m68k.cpu.a[7];
            for (int _i = 0; _i < 6; _i++)
                g_sound.dbg_cmd1_stk[_i] = g_sound.m68k.read_cb(g_sound.m68k.mem_ctx, sp + (uint32_t)_i*4u, 4);
            g_sound.dbg_cmd1_done = 1;
        }
        else if (pc == 0x603B12u) {
            g_sound.dbg_sc07++; g_sound.dbg_sc07_d0 = (uint8_t)g_sound.m68k.cpu.d[0];
            if (!g_sound.dbg_sc07_stk_done) {
                uint32_t sp = g_sound.m68k.cpu.a[7];
                for (int _i = 0; _i < 6; _i++)
                    g_sound.dbg_sc07_stk[_i] = g_sound.m68k.read_cb(g_sound.m68k.mem_ctx, sp + (uint32_t)_i*4u, 4);
                g_sound.dbg_sc07_stk_done = 1;
            }
        }
        else if (pc == 0x603BB4u) g_sound.dbg_sc07_post++;
        else if (pc == 0x603A52u) g_sound.dbg_sc05++;
        else if (pc == 0x601884u) g_sound_noteon++;   /* DIAG: Interrupt1 (Timer A) — pc after entry movem; reusing noteon field */
        else if (pc == 0x601916u) g_sound_noteoff++;  /* DIAG: Interrupt1 duration key-off path (after #$10 write); reusing noteoff field */
        else if (pc == 0x6027FEu) g_sound_noteoff++;  /* note-off total (0x8x entry + vel-0 redirect) */
        else if (pc == 0x60379Eu) g_sound.dbg_c3++;   /* SndCmd00 sub[3] loc_60379E */
        else if (pc == 0x603856u) g_sound.dbg_c4++;   /* SndCmd00 sub[4] loc_603856 (start music) */
        {
            static const uint32_t ENQ_PC[11] = {0x60182au,0x601858u,0x6038d0u,0x603b66u,
                0x603faau,0x604022u,0x6040e6u,0x60412cu,0x60416cu,0x6041aau,0x6041d6u};
            for (int _e = 0; _e < 11; _e++) if (pc == ENQ_PC[_e]) { g_sound.dbg_enq[_e]++; break; }
        }
        /* MAME runs the Int2 ISR's Timer-B path (track @A6+0x2000, SCIPD bit 7)
         * AND Timer-C path (6 BGM tracks @A6+0x2010.., SCIPD bit 8) at EQUAL,
         * ALTERNATING rates (≈1:1).  Fire at half the slice interval and toggle
         * the SCIPD bit each time, so each path runs at the sequencer rate and
         * the two interleave like hardware.  (We used to fire only bit 7, so the
         * 6 Timer-C tracks were activated but never processed → their note-ons
         * never keyed off → stuck/overlapping voices.) */
        if (!g_sound_int2_disable) {
            int half = int2_interval / 2; if (half < 1) half = 1;
            if (++g_sound_timer_cnt >= (uint32_t)half) {
                g_sound_timer_cnt = 0;
                if (g_sound_timer_cnt_c ^= 1u) g_sound.comm[0x421] |= 0x80; /* Timer B (bit 7) */
                else                           g_sound.comm[0x420] |= 0x01; /* Timer C (bit 8) */
                m68k_interrupt(&g_sound.m68k, 2);
            }
            /* Timer A → 68K level-1 IRQ (Interrupt1 @0x601880): the per-voice
             * DURATION timer. It decrements each voice's gate counter and keys
             * the voice off (#$10 → SCSP) when it expires. Without this, note
             * durations never elapse → voices never key off ("loops not ending").
             * Rate from TIMA (comm[0x418] presc / comm[0x419] reload, ~86 Hz). */
            {
                uint32_t presc_a  = g_sound.comm[0x418] & 7u;
                uint32_t reload_a = g_sound.comm[0x419];
                if (reload_a < 255u) {
                    double hz_a = 22579200.0 / (512.0 * (double)(1u << presc_a)
                                                * (double)(255u - reload_a));
                    /* fires per 60Hz slice = hz_a/60 (~1.46); divide without
                     * rounding to integer-per-slice so the rate isn't lost. */
                    int int_a = (int)((double)n / (hz_a / 60.0));
                    if (int_a < 1) int_a = 1;
                    if (++g_sound_timer_cnt_a >= (uint32_t)int_a) {
                        g_sound_timer_cnt_a = 0;
                        g_sound.comm[0x421] |= 0x40;  /* assert SCIPD bit 6 = Timer A pending */
                    }
                }
                /* Deliver the level-1 IRQ as a HELD line: retry every step until
                 * the 68K's IPL drops below 1 (it's masked while a level-2 ISR
                 * runs). m68k_interrupt no-ops when masked; the Interrupt1
                 * handler clears the pending bit via its SCIRE #$40 write. */
                if (g_sound.comm[0x421] & 0x40u)
                    m68k_interrupt(&g_sound.m68k, 1);
            }
        }
    }
    g_sound_step_total += (uint32_t)done;
    static uint32_t s_next_log = 1000000u;
    if (g_sound_step_total >= s_next_log) {
        s_next_log += 5000000u;
        uint32_t vec2 = buf_read(g_sound.wave, 0x68, 4); /* Timer B handler */
        uint32_t vec3 = buf_read(g_sound.wave, 0x6C, 4); /* MIDI handler    */
        LOG_INFO("sound: 68K @%u steps pc=0x%06X stopped=%d "
                 "vec2=0x%06X vec3=0x%06X midi_writes=%llu scsp_loaded=%d",
                 g_sound_step_total, g_sound.m68k.cpu.pc,
                 g_sound.m68k.cpu.stopped, vec2, vec3,
                 (unsigned long long)g_sound.write_count, g_scsp.loaded);
    }
    return done;
}

#endif /* SOUND_H */
