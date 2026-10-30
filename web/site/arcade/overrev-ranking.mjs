// Over Rev's RANKINGs, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.overrev). Read out of the
// Model 2C Revision A program (epr-20124a/20125a, MAME's overrev) (Pinboard #1911); the
// Model 2B Revision B (overrevb) addresses the backup the same way, and MAME writes the
// same factory board for both (only the counters at 0x00-0x01 differ).
//
// One block of 0x1AEA bytes at 0x0E, led by a u16 LE CRC at 0x0C. The CRC is the
// program's (0x14c60): poly 0x1021 in a 32-bit register seeded 0xDEBDEB00, the upper half
// taken after the last byte (see crc16 below). The code keeps room for a second copy
// (CRC at 0x1AF8, data from 0x1AFA) but never writes it: at boot (0x144a0) a good block
// is signed again (0x14560), a bad one is replaced by a good second copy (0x14710) or else
// everything up to 0x3617 is cleared and filled with the factory tables (0x14580).
// 0x00-0x0B are counters (0x9e50); 0x35E4-0x3610 lie outside the block.
//
// A ranking block of 0x358 bytes sits at 0x38 + 0xD60 * mode + 0x358 * course: mode 0 is
// TIME ATTACK, one per course (TSUKUBA, SHIBUYA, HAKONE, ARIAKE: 0x38, 0x390, 0x6E8,
// 0xA40, as the attract screens show them); mode 1 is the CHALLENGE CUP, which the game
// always files under course 0 (0x0adc0), so only 0xD98 is used (the factory fill also
// writes 0x10F0, 0x1448 and 0x17A0; nothing reads them). Block +0 and +4 are u32s, +0x328
// on play bookkeeping (count, total, shortest, longest, a histogram). The ranking is 10
// entries of 20 bytes at block + 8, best first; 0x0adc0/0xb95c insert a finish before the
// first entry it beats (a tie goes after) and shift the rest down:
//   +0   u32 LE  time in frames (1/60 s), shown mm'ss"cc; the factory 215940 is 59'59"00
//   +4   u32 LE  0 in the factory tables
//   +8   u8  car 0-3 (the factory picks them at random)
//   +9   u8  from 0x595a38 (0 in the factory tables)
//   +10  u8  0 MT (manual), 1 AT, shown beside the car
//   +11  2 bytes, 0 in the factory tables
//   +13  6 bytes the name, a code each: 0-25 A-Z, 26-35 0-9, 0x24 '!', 0x25 '?', 0x26 '@',
//        0x27 '&', 0x28 '/', 0x29 '.', 0x2C '_' (the name entry's blank). 0x2A and 0x2B are
//        the name entry's back and END keys; from 0x2D up the font has no letters.
//        (Shown on the attract ranking from a poked board.) Default names: ROM 0xe9d26.
//   +19  0
//
// Checked in MAME: a board written as below boots with its entry on the ranking screen
// (the CRC holds).

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'overrev';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 20;
export const ENTRIES = 10;
export const CRC_AT = 0x0C;
export const COPY_SIZE = 0x1AEC;            // the CRC and the 0x1AEA bytes it covers
export const COPIES = [CRC_AT, CRC_AT + COPY_SIZE];
export const TABLES = ['Time attack: Tsukuba', 'Time attack: Shibuya', 'Time attack: Hakone',
  'Time attack: Ariake', 'Challenge cup'];
const BLOCKS = [0x38, 0x390, 0x6E8, 0xA40, 0xD98];   // in the first copy
const RANKING_IN_BLOCK = 8;

export const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!?@&/.\0\0_';
const MAX_TIME = 216000;                     // 60'00"00: two digits of minutes
const CARS = 4;

/* The program's 16-bit CRC (0x14c60). */
export function crc16(bytes) {
  let g = 0xDEBDEB00;
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) >>> 0) + x;
    for (let i = 0; i < 8; i++) g = g > 0x7FFFFFFF ? ((g << 1) + 0x10210000) >>> 0 : (g << 1) >>> 0;
  }
  return g >>> 16;
}

const u16 = (b, at) => b[at] | b[at + 1] << 8;
const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const nameChar = (c) => (c < CHARS.length && CHARS[c] !== '\0' ? CHARS[c] : null);

/* One 20-byte entry -> { time, initials (the 6-letter name), car, transmission, hex }, or null. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[19] !== 0) return null;
  if (bytes[8] >= CARS || bytes[10] > 1) return null;
  const time = u32(bytes, 0);
  if (time === 0 || time >= MAX_TIME) return null;
  let initials = '';
  for (let i = 13; i < 19; i++) {
    const c = nameChar(bytes[i]);
    if (c === null) return null;
    initials += c;
  }
  return { time, initials, car: bytes[8], transmission: bytes[10], hex: bytesToHex(bytes) };
}

/* True when a copy carries its own CRC, as the game checks it. */
export function copyValid(backup, copy) {
  return u16(backup, copy) === crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
}

const tableAt = (copy, t) => BLOCKS[t] - COPIES[0] + copy + RANKING_IN_BLOCK;

/* The five tables, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  return TABLES.map((_, t) => {
    const out = [];
    for (let i = 0; i < ENTRIES; i++) {
      const at = tableAt(copy, t) + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 10 each) into the block and signs it again. The second
 * copy stays as it is (the game never writes it), unless it is the good one: then it is
 * copied over a bad first, as the game does at boot, before the write. */
export function writeTables(backup, tables) {
  const [a, b] = COPIES;
  if (!copyValid(backup, a) && copyValid(backup, b)) backup.copyWithin(a, b, b + COPY_SIZE);
  tables.forEach((table, t) => {
    if (t >= TABLES.length) return;
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      backup.set(hexToBytes(table[i].hex), tableAt(a, t) + i * ENTRY_SIZE);
    }
  });
  const c = crc16(backup.subarray(a + 2, a + COPY_SIZE));
  backup[a] = c & 0xFF;
  backup[a + 1] = c >> 8;
  return backup;
}

/* Frames as the game shows a time: mm'ss"cc. */
export function formatTime(frames) {
  const cs = Math.floor((frames % 60) * 100 / 60), s = Math.floor(frames / 60) % 60;
  return `${String(Math.floor(frames / 3600)).padStart(2, '0')}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}

export function detail(entry) {
  return entry ? `Car ${entry.car + 1} ${entry.transmission ? 'AT' : 'MT'}` : '';
}
