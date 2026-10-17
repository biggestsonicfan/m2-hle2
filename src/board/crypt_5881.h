/*
 * crypt_5881.h — the Sega 315-5881 decryption chip (board-level).
 *
 * A port of MAME's sega_315_5881_crypt_device (src/mame/sega/315-5881_crypt.cpp,
 * BSD-3-Clause, Andreas Naive, Olivier Galibert, David Haywood): the cipher, its
 * tables and the decompressor are MAME's, line for line. Read MAME's notes there
 * for how the cipher works.
 *
 * On Model 2 (MAME model2_5881_mem; 2A, 2B and 2C alike) the game writes the
 * enciphered words into 64 KB of RAM at 0x01D80000, points the chip at them and
 * reads the plain words back out of its port at 0x01D90000. The port is 16 bits
 * a register, little endian (iomap_le):
 *
 *   +0x0  read   ready (bit 0 busy: never)
 *   +0x8  write  address, low half (a word index into the RAM)
 *   +0xA  write  address, high half (always 0 on Model 2)
 *   +0xC  write  subkey
 *   +0xE  read   the next plain word; the first read after an address write
 *                is a dummy 0
 *
 * The key is the game's (the ROM_PARAMETER ":315_5881:key" of its ROM_START).
 * Dynamite Cop, Pilot Kids and Zero Gunner 2 use it.
 */
#ifndef CRYPT_5881_H
#define CRYPT_5881_H

#include <stdint.h>
#include <string.h>

#include "log.h"
#include "memory.h"

#define C5881_RAM_BASE   0x01D80000u
#define C5881_RAM_SIZE   0x00010000u
#define C5881_PORT_BASE  0x01D90000u
#define C5881_PORT_SIZE  0x00010000u
#define C5881_BUFFER     2
#define C5881_LINE       512
#define C5881_COMPRESSED 0x20000u
#define C5881_FN1GK      38
#define C5881_FN2GK      32

typedef struct {
    uint8_t table[64];
    int inputs[6];      /* positions of the input bits, -1: none but the key */
    int outputs[2];     /* positions of the output bits */
} c5881_sbox_t;

/* Everything a board reset clears and a savestate keeps. */
typedef struct {
    uint32_t key;
    uint8_t  ram[C5881_RAM_SIZE];
    uint8_t  buffer[C5881_BUFFER];
    uint8_t  line[2][C5881_LINE];   /* the line being read, the line before */
    int32_t  line_cur;              /* which of line[] is line_buffer */
    uint32_t prot_cur_address;
    uint16_t subkey, dec_hist;
    uint32_t dec_header;
    int32_t  enc_ready, first_read;
    int32_t  buffer_pos, line_buffer_pos, line_buffer_size, buffer_bit, buffer_bit2;
    uint8_t  buffer2[2];
    uint16_t buffer2a;
    int32_t  block_size, block_pos, block_numlines, done_compression;
} crypt_5881_t;

static crypt_5881_t g_c5881;

static const c5881_sbox_t c5881_fn1_sboxes[4][4] = {
    {   // 1st round
        {
            {
                0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
                2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
            },
            {3,4,5,7,-1,-1},
            {0,4}
        },

        {
            {
                2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
                0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
            },
            {0,1,2,5,6,7},
            {1,6}
        },

        {
            {
                0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
                1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
            },
            {0,2,5,6,7,-1},
            {2,3}
        },

        {
            {
                3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
                1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
            },
            {1,2,3,4,6,7},
            {5,7}
        },
    },
    {   // 2nd round
        {
            {
                3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
                2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
            },
            {0,1,3,4,5,7},
            {0,4}
        },

        {
            {
                2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
                3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
            },
            {0,1,3,4,6,-1},
            {1,5}
        },

        {
            {
                2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
                3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
            },
            {1,2,5,6,7,6},
            {2,7}
        },

        {
            {
                0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
                0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
            },
            {2,3,4,5,6,7},
            {3,6}
        },
    },
    {   // 3rd round
        {
            {
                0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
                0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
            },
            {1,2,3,4,5,7},
            {0,5}
        },

        {
            {
                1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
                1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
            },
            {0,2,4,5,6,7},
            {1,6}
        },

        {
            {
                0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
                // unused?
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {0,2,4,6,7,-1},
            {2,3}
        },

        {
            {
                0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
                2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
            },
            {0,1,2,3,6,-1},
            {4,7}
        },
    },
    {   // 4th round
        {
            {
                0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
                3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
            },
            {0,1,3,4,5,6},
            {0,5}
        },

        {
            {
                0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
                3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
            },
            {0,1,2,3,5,7},
            {1,7}
        },

        {
            {
                0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
                3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
            },
            {2,3,4,5,6,7},
            {2,3}
        },

        {
            {
                0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
                1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
            },
            {0,1,2,4,6,7},
            {4,6}
        },
    },
};


static const c5881_sbox_t c5881_fn2_sboxes[4][4] = {
    {   // 1st round
        {
            {
                3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
                2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
            },
            {1,3,4,5,6,7},
            {0,7}
        },

        {
            {
                0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
                3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
            },
            {0,3,5,6,5,0},
            {1,2}
        },

        {
            {
                0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
                0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
            },
            {0,2,3,4,7,-1},
            {3,4}
        },

        {
            {
                2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
                2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
            },
            {1,2,5,6,-1,-1},
            {5,6}
        },
    },
    {   // 2nd round
        {
            {
                2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
                2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
            },
            {0,1,4,5,6,7},
            {0,7}
        },

        {
            {
                0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
                1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
            },
            {0,2,3,6,7,-1},
            {1,5}
        },

        {
            {
                1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
                1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
            },
            {2,3,4,6,7,2},
            {2,3}
        },

        {
            {
                2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
                3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
            },
            {1,2,3,4,5,6},
            {4,6}
        },
    },
    {   // 3rd round
        {
            {
                0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
                3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
            },
            {1,4,5,6,7,5},
            {0,5}
        },

        {
            {
                0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
                2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
            },
            {0,2,3,4,6,7},
            {1,7}
        },

        {
            {
                2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
                // unused?
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {1,2,4,7,2,-1},
            {2,4}
        },

        {
            {
                0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
                3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
            },
            {0,1,2,3,5,7},
            {3,6}
        },
    },
    {   // 4th round
        {
            {
                0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
                0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
            },
            {0,1,2,4,7,-1},
            {0,5}
        },

        {
            {
                2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
                0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
            },
            {0,1,3,5,6,-1},
            {1,3}
        },

        {
            {
                0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
                3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
            },
            {0,2,3,4,5,7},
            {2,7}
        },

        {
            {
                0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
                2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
            },
            {2,3,4,5,6,7},
            {4,6}
        },
    },
};

static const int c5881_fn1_game_key_scheduling[C5881_FN1GK][2] = {
    {1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
    {6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
    {12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
    {19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
    {25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

static const int c5881_fn2_game_key_scheduling[C5881_FN2GK][2] = {
    {0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
    {8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
    {14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
    {22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

static const int c5881_fn1_sequence_key_scheduling[20][2] = {
    {0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
    {6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
    {13,74}, {14,19}, {14,80}, {15,26}
};

static const int c5881_fn2_sequence_key_scheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};

static const int c5881_fn2_middle_result_scheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

static const uint8_t c5881_trees[9][2][32] = {
    {
        {0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
            0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
            0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
            0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff,},
        {0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
            0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff,},
    },
    {
        {0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
            0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
            0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
            0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
            0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
            0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
            0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
};

#define C5881_BIT(x, n) (((x) >> (n)) & 1)

static inline int c5881_feistel(int input, const c5881_sbox_t *sboxes, uint32_t subkeys) {
    int result = 0;
    for (int m = 0; m < 4; ++m) {
        int aux = 0;
        for (int k = 0; k < 6; ++k)
            if (sboxes[m].inputs[k] != -1)
                aux |= C5881_BIT(input, sboxes[m].inputs[k]) << k;
        aux = sboxes[m].table[(aux ^ subkeys) & 0x3f];
        for (int k = 0; k < 2; ++k)
            result |= C5881_BIT(aux, k) << sboxes[m].outputs[k];
        subkeys >>= 6;
    }
    return result;
}

/* MAME's bitswap<16>(v, b15 .. b0): bit 15 of the result is bit b15 of v. */
static inline int c5881_bitswap16(int v, const uint8_t bits[16]) {
    int r = 0;
    for (int i = 0; i < 16; i++) r |= C5881_BIT(v, bits[i]) << (15 - i);
    return r;
}

static inline void c5881_schedule(uint32_t sub[4], int pos) {
    sub[pos / 24] ^= 1u << (pos % 24);
}

static inline int c5881_network(int aux, const c5881_sbox_t sboxes[4][4], const uint32_t sub[4]) {
    int B = aux >> 8;
    int A = (aux & 0xff) ^ c5881_feistel(B, sboxes[0], sub[0]);
    B ^= c5881_feistel(A, sboxes[1], sub[1]);
    A ^= c5881_feistel(B, sboxes[2], sub[2]);
    B ^= c5881_feistel(A, sboxes[3], sub[3]);
    return (B << 8) | A;
}

static inline uint16_t c5881_block_decrypt(uint32_t game_key, uint16_t sequence_key,
                                           uint16_t counter, uint16_t data) {
    static const uint8_t swap_counter[16] = {5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2};
    static const uint8_t swap_data[16]    = {14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10};
    static const uint8_t swap_out[16]     = {15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8};
    uint32_t fn1[4] = {0, 0, 0, 0}, fn2[4] = {0, 0, 0, 0};

    for (int j = 0; j < C5881_FN1GK; ++j)
        if (C5881_BIT(game_key, c5881_fn1_game_key_scheduling[j][0]))
            c5881_schedule(fn1, c5881_fn1_game_key_scheduling[j][1]);
    for (int j = 0; j < C5881_FN2GK; ++j)
        if (C5881_BIT(game_key, c5881_fn2_game_key_scheduling[j][0]))
            c5881_schedule(fn2, c5881_fn2_game_key_scheduling[j][1]);
    for (int j = 0; j < 20; ++j)
        if (C5881_BIT(sequence_key, c5881_fn1_sequence_key_scheduling[j][0]))
            c5881_schedule(fn1, c5881_fn1_sequence_key_scheduling[j][1]);
    for (int j = 0; j < 16; ++j)
        if (C5881_BIT(sequence_key, j))
            c5881_schedule(fn2, c5881_fn2_sequence_key_scheduling[j]);

    int middle = c5881_network(c5881_bitswap16(counter, swap_counter), c5881_fn1_sboxes, fn1);
    for (int j = 0; j < 16; ++j)
        if (C5881_BIT(middle, j))
            c5881_schedule(fn2, c5881_fn2_middle_result_scheduling[j]);

    int aux = c5881_network(c5881_bitswap16(data, swap_data), c5881_fn2_sboxes, fn2);
    return (uint16_t)c5881_bitswap16(aux, swap_out);
}

/* MAME model2_state::crypt_read_callback: a 16-bit word of the RAM, bytes
 * swapped. The word index wraps in the 64 KB. */
static inline uint16_t c5881_read(uint32_t addr) {
    uint32_t off = (addr * 2u) & (C5881_RAM_SIZE - 2u);
    return (uint16_t)(g_c5881.ram[off] << 8 | g_c5881.ram[off + 1]);
}

static inline uint16_t c5881_get_decrypted_16(void) {
    crypt_5881_t *c = &g_c5881;
    uint16_t enc = c5881_read(c->prot_cur_address);
    uint16_t dec = c5881_block_decrypt(c->key, c->subkey, (uint16_t)c->prot_cur_address, enc);
    uint16_t res = (uint16_t)((dec & 3) | (c->dec_hist & 0xfffc));
    c->dec_hist = dec;
    c->prot_cur_address++;
    return res;
}

static inline void c5881_enc_start(void) {
    crypt_5881_t *c = &g_c5881;
    c->block_pos = 0;
    c->done_compression = 0;
    c->buffer_pos = C5881_BUFFER;
    if (c->buffer_bit2 < 14) {
        c->dec_header = (uint32_t)(c->buffer2a & 0x0003) << 16;
    } else {
        c->dec_hist = 0;
        c->dec_header = (uint32_t)c5881_get_decrypted_16() << 16;
    }
    c->dec_header |= c5881_get_decrypted_16();

    c->block_numlines = (int)(c->dec_header & 0xff) + 1;
    int blocky = (int)((c->dec_header & 0x1ff00) >> 8) + 1;
    c->block_size = c->block_numlines * blocky;
    if (c->dec_header & C5881_COMPRESSED) {
        c->line_buffer_size = blocky;
        c->line_buffer_pos = c->line_buffer_size;
        c->buffer_bit = 7;
        c->buffer_bit2 = 15;
    }
    c->enc_ready = 1;
}

static inline void c5881_enc_fill(void) {
    crypt_5881_t *c = &g_c5881;
    for (int i = 0; i != C5881_BUFFER; i += 2) {
        uint16_t val = c5881_get_decrypted_16();
        c->buffer[i] = (uint8_t)val;
        c->buffer[i + 1] = (uint8_t)(val >> 8);
        c->block_pos += 2;
        if (!(c->dec_header & C5881_COMPRESSED) && c->block_pos == c->block_size)
            c5881_enc_start();
    }
    c->buffer_pos = 0;
}

static inline int c5881_get_compressed_bit(void) {
    crypt_5881_t *c = &g_c5881;
    if (c->buffer_bit2 == 15) {
        c->buffer_bit2 = 0;
        c->buffer2a = c5881_get_decrypted_16();
        c->buffer2[0] = (uint8_t)c->buffer2a;
        c->buffer2[1] = (uint8_t)(c->buffer2a >> 8);
        c->buffer_pos = 0;
    } else {
        c->buffer_bit2++;
    }
    int res = (c->buffer2[(c->buffer_pos & 1) ^ 1] >> c->buffer_bit) & 1;
    if (--c->buffer_bit == -1) {
        c->buffer_bit = 7;
        c->buffer_pos++;
    }
    return res;
}

static inline uint8_t c5881_get_compressed_byte(void) {
    uint8_t byte = 0;
    for (int b = 0; b < 8; b++) byte = (uint8_t)(byte << 1 | c5881_get_compressed_bit());
    return byte;
}

/* One line of a compressed block: each run either repeats a byte from the
 * stream or copies the line before, shifted by -1, 0 or +1 byte. */
static inline void c5881_line_fill(void) {
    static const int offsets[4] = {0, 1, 0, -1};
    crypt_5881_t *c = &g_c5881;
    const uint8_t *lp = c->line[c->line_cur];
    c->line_cur ^= 1;
    uint8_t *lc = c->line[c->line_cur];
    int size = c->line_buffer_size;

    c->line_buffer_pos = 0;
    for (int i = 0; i != size;) {
        /* vlc 0: start of line, 1: interior, 2-9: 7-1 bytes from the end */
        int slot = i ? i < size - 7 ? 1 : (i & 7) + 1 : 0;
        uint32_t tmp = 0;
        while (!(tmp & 0x80))
            tmp = c5881_trees[slot][c5881_get_compressed_bit()][tmp];
        if (tmp == 0xff) continue;
        int count = (int)(tmp & 7) + 1;
        if (tmp & 0x40) {
            int offset = offsets[(tmp & 0x18) >> 3];
            for (int j = 0; j != count; j++, i++)
                lc[i ^ 1] = lp[((i + offset) % size) ^ 1];
        } else {
            uint8_t byte = c5881_get_compressed_byte();
            for (int j = 0; j != count; j++) lc[(i++) ^ 1] = byte;
        }
    }
    if (c->block_numlines == ++c->block_pos) c->done_compression = 1;
}

static inline uint16_t c5881_do_decrypt(void) {
    crypt_5881_t *c = &g_c5881;
    const uint8_t *base;
    if (!c->enc_ready) c5881_enc_start();
    if (c->dec_header & C5881_COMPRESSED) {
        if (c->line_buffer_pos == c->line_buffer_size) {
            if (c->done_compression == 1) c5881_enc_start();
            c5881_line_fill();
        }
        base = c->line[c->line_cur] + c->line_buffer_pos;
        c->line_buffer_pos += 2;
    } else {
        if (c->buffer_pos == C5881_BUFFER) c5881_enc_fill();
        base = c->buffer + c->buffer_pos;
        c->buffer_pos += 2;
    }
    return (uint16_t)(base[0] << 8 | base[1]);
}

/* decrypt_le_r: decrypt_be_r with its bytes swapped. */
static inline uint16_t c5881_decrypt_le(void) {
    uint16_t v = 0;
    if (g_c5881.first_read) g_c5881.first_read = 0;
    else                    v = c5881_do_decrypt();
    return (uint16_t)(v >> 8 | v << 8);
}

static inline void c5881_reg_w(uint32_t off, uint16_t v) {
    crypt_5881_t *c = &g_c5881;
    switch (off & 0xFFFEu) {
    case 0x8:   /* addrlo_w */
        c->prot_cur_address = (c->prot_cur_address & 0xffff0000u) | v;
        c->enc_ready = 0;
        c->first_read = 1;
        break;
    case 0xA:   /* addrhi_w: the high half is always set to 0 */
        if (v) LOG_WARN("315-5881: high address 0x%04X", v);
        c->prot_cur_address &= 0x0000ffffu;
        c->enc_ready = 0;
        c->buffer_bit = 7;
        c->buffer_bit2 = 15;
        c->first_read = 1;
        break;
    case 0xC:   /* subkey_le_w: a request, one a table */
        c->subkey = v;
        c->enc_ready = 0;
        LOG_DEBUG("315-5881: subkey 0x%04X at 0x%05X", v, c->prot_cur_address);
        break;
    default:
        LOG_WARN("315-5881: write 0x%04X to +0x%X", v, off);
        break;
    }
}

static inline uint16_t c5881_reg_r(uint32_t off) {
    if ((off & 0xFFFEu) == 0xE) return c5881_decrypt_le();
    return 0;   /* +0x0 ready: never busy; the rest reads nothing */
}

/* A 32-bit access is the two 16-bit registers it covers, low half first. */
static uint32_t c5881_port_read_cb(mem_region_t *r, uint32_t addr, int size) {
    uint32_t off = (addr - r->base) & 0xFu;
    if (size == 4) return c5881_reg_r(off) | (uint32_t)c5881_reg_r(off + 2) << 16;
    uint32_t v = c5881_reg_r(off);
    return size == 1 ? (v >> ((off & 1) * 8)) & 0xFFu : v;
}

static void c5881_port_write_cb(mem_region_t *r, uint32_t addr, uint32_t val, int size) {
    uint32_t off = (addr - r->base) & 0xFu;
    if (size == 4) {
        c5881_reg_w(off, (uint16_t)val);
        c5881_reg_w(off + 2, (uint16_t)(val >> 16));
    } else if (size == 2) {
        c5881_reg_w(off, (uint16_t)val);
    } else {
        LOG_WARN("315-5881: byte write 0x%02X to +0x%X", val & 0xFFu, off);
    }
}

/* Clear the chip (MAME device_reset) and map its RAM and port. Called by a
 * profile's install, after mem_init. */
static inline void crypt_5881_install(memory_bus_t *bus, uint32_t key) {
    memset(&g_c5881, 0, sizeof g_c5881);
    g_c5881.key = key;
    mem_region_t *ram = mem_add_region(bus, "C5881_RAM", C5881_RAM_BASE, C5881_RAM_SIZE, g_c5881.ram, 0);
    (void)ram;
    mem_region_t *r = mem_add_region(bus, "C5881", C5881_PORT_BASE, C5881_PORT_SIZE, NULL, 0);
    if (r) {
        r->read_cb  = c5881_port_read_cb;
        r->write_cb = c5881_port_write_cb;
        r->no_burst = 1;
    }
    mem_regions_changed(bus);
}

#endif /* CRYPT_5881_H */
