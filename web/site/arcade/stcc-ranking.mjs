// Sega Touring Car Championship's best times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like ranking.mjs (Sonic the Fighters'). m2-hle2's
// web build keeps the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.stcc).
// Read out of the stcca program (Revision A, epr-19272a/19273a: the set the container's
// stcc.zip holds, and the one m2-hle2's stcc profile runs) for Pinboard #1911:
//
// The game keeps two copies of a 0x490-byte block, at 0 and 0x490. Each starts with a
// CRC-16/GENIBUS (poly 0x1021, init and xorout 0xFFFF; 0x66ec0 -> the table routine at
// 0x7a700, table at 0x7a270) as a u16 LE over [+2, +0x490). The boot check (0x67100)
// keeps copy 0 when it is good (copy 1 is not compared with it), copies 1 over 0 when
// only 1 is good, and when both are bad refills the factory data (0x66e70 -> 0x540d0),
// signs copy 0 and copies it to 1. Nothing else in the block is checked.
//
// The block, at copy offsets:
//   +0x088, +0x0A8  8 floats each (factory 38, 40, 60, 60, 38, 39, 60, 60; ROM 0x53fb0)
//   +0x0C8          ranking 0: 8 entries of 16 bytes, best first (factory: ROM 0x53fd0)
//   +0x148          ranking 1: the same (factory: ROM 0x54050)
//   +0x1C8 / +0x1CC a toggle, then the 8 cars' settings (0x58 bytes each)
//   +0x48C          u32: nonzero once the test menu's car settings were changed (0x652c0);
//                   the race end (0x5704) then skips the ranking. The factory reset clears it.
// An entry (insertion at 0x1c340, called from 0x5728 after the last race with the total
// time from 0x2022e8; lower wins, by a float compare):
//   +0  f32 LE  total time in seconds (factory 186.005 ... 235.909)
//   +4  8 chars name: the game fills them with '.', then copies 7 letters (3 when the
//       test menu's NAME ENTRY is one of the "_3" settings); the factory names end in NUL
//   +12 u16 LE  bits 3-5 the car (0..7, 0x204430), bit 6 a pad bit (0x204434, input
//       0x202f94 bit 7: the gear shift type, it seems), bits 7-8 a 0-3 choice made on the
//       car select (0x204438); the other bits are kept as they were
//   +14 u16     zero
// Which table: 0x204720 = (0x20230c != 0), and 0x20230c is set (0x3ad0) when the course
// number 0x204410 is 3, 4 or 5: the SS courses that follow NOVICE, INTERMEDIATE and EXPERT
// (names at 0x15b80, "SS MODE" at 0x195c8). The attract shows table 1 while a View button
// is held (0x31e0c).
//
// This module reports the time in milliseconds (the service's `time` is an integer) and
// writes the entry's own 16 bytes back, so nothing is lost by rounding.
// Played in MAME: first boot only (the factory board, seed/stcc-backup1). The drive
// board ROM epr-18261.ic9 is missing in the container and was replaced by zeros. Not
// checked: the later stcc revision and stccb/stcco (their ROMs are not here).

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'stcc';
export const NVRAM_KEY = 'm2hle2.nvram.stcc';
export const ENTRY_SIZE = 16;
export const ENTRIES = 8;
export const COPY_SIZE = 0x490;
export const COPIES = [0, COPY_SIZE];
export const TABLES_AT = [0x0C8, 0x148];
export const TABLES = ['Championship', 'Championship (SS courses)'];

// The test menu's names (0x65030); the game's own order of its cars is not confirmed.
export const CARS = ['Alfa Romeo', 'Mercedes', 'Opel', 'Toyota', 'Alfa Romeo 2', 'Mercedes 2', 'Opel 2', 'Toyota 2'];

const NAME = /^[\x20-\x5f]{7}$/;
const MAX_TIME = 3600;    // seconds: a bound on what a real entry holds

export function crc16(bytes) {
  let c = 0xFFFF;
  for (const x of bytes) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c ^ 0xFFFF;
}

const u16 = (b, at) => b[at] | b[at + 1] << 8;

/* One 16-byte entry -> { time (ms), initials, car, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  if (bytes[11] !== 0 && bytes[11] !== 0x2E) return null;
  const seconds = new DataView(bytes.buffer, bytes.byteOffset, 4).getFloat32(0, true);
  if (!Number.isFinite(seconds) || seconds <= 0 || seconds >= MAX_TIME) return null;
  const name = String.fromCharCode(...bytes.subarray(4, 11));
  if (!NAME.test(name)) return null;
  const bits = u16(bytes, 12);
  return {
    time: Math.round(seconds * 1000),
    initials: name.replace(/\.+$/, '') || name,
    car: (bits >> 3) & 7,
    hex: bytesToHex(bytes),
  };
}

/* True when a copy carries its own CRC, as the game checks it. */
export function copyValid(backup, copy) {
  return u16(backup, copy) === crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
}

/* The two tables, from the copy the game would keep; none when both copies are bad
   (the game would put the factory times back). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c));
  return TABLES_AT.map((at) => {
    const out = [];
    if (copy === undefined) return out;
    for (let i = 0; i < ENTRIES; i++) {
      const e = parseEntry(backup.subarray(copy + at + i * ENTRY_SIZE, copy + at + (i + 1) * ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 8 each) into both copies and signs both again. */
export function writeTables(backup, tables) {
  for (const copy of COPIES) {
    TABLES_AT.forEach((at, t) => {
      const table = tables[t] || [];
      for (let i = 0; i < ENTRIES && i < table.length; i++) {
        backup.set(hexToBytes(table[i].hex), copy + at + i * ENTRY_SIZE);
      }
    });
    const c = crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
    backup[copy] = c & 0xFF;
    backup[copy + 1] = c >> 8;
  }
  return backup;
}

/* m'ss"cc from milliseconds (hundredths cut, not rounded; the game's own display
   routine was not traced). */
export function formatTime(ms) {
  const cs = Math.floor(ms / 10), s = Math.floor(cs / 100) % 60;
  return `${Math.floor(cs / 6000)}'${String(s).padStart(2, '0')}"${String(cs % 100).padStart(2, '0')}`;
}

export function detail(entry) { return entry && entry.car !== undefined ? CARS[entry.car] || '' : ''; }
