// Sega Rally Championship's best times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like ranking.mjs (Sonic the Fighters'). m2-hle2's
// web build keeps the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.srallyc).
// Read out of the srallyc program (Pinboard #654):
//
// The game keeps two copies of a 0xC68-byte block, at 0 and 0xC68, and checks both at
// boot (0x2c80): a bad copy is replaced by the good one, and two bad copies are wiped back
// to the factory times. Each copy has three sections, each led by a CRC-16/GENIBUS
// (poly 0x1021, init and xorout 0xFFFF; the routine at 0x297e0) as a u16 LE:
//   CRC +0x000 over [+0x002, +0x148)   (+0x002 holds the block's length, 0x0C68)
//   CRC +0x14C over [+0x150, +0x2E8)   (+0x148: "section changed", recompute)
//   CRC +0x2EC over [+0x2F0, +0xC68)   (+0x2E8: the same for this one)
// The third section holds six records of 0x194 bytes from +0x2F0. A record's ranking is
// 16 entries of 12 bytes at +0xD4, best first:
//   +0  u32 LE  time in frames (1/60 s): the factory 12600 shows as 3'30"00
//   +4  3 chars initials, then a NUL
//   +8  u32 LE  car: bit 0 manual (else automatic), bit 1 Delta (else Celica)

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const NVRAM_KEY = 'm2hle2.nvram.srallyc';
export const ENTRY_SIZE = 12;
export const ENTRIES = 16;
export const COPY_SIZE = 0xC68;
export const COPIES = [0, COPY_SIZE];

const RECORDS_AT = 0x2F0, RECORD_SIZE = 0x194, RANKING_IN_RECORD = 0xD4;
const SECTIONS = [[0x000, 0x002, 0x148], [0x14C, 0x150, 0x2E8], [0x2EC, 0x2F0, COPY_SIZE]];

// Record 0 is the championship's; 1-4 are the courses in the game's own order (its
// strings at 0x9b90). Record 5 is a second championship table, picked by a flag
// (0x2020b4) whose meaning is not known yet; the factory fills it like record 0.
export const TABLES = ['Championship', 'Desert', 'Forest', 'Mountain', 'Lakeside', 'Championship (2)'];

const INITIAL = /^[\x20-\x5f]{3}$/;
const CARS = ['Celica AT', 'Celica MT', 'Delta AT', 'Delta MT'];

export function crc16(bytes) {
  let c = 0xFFFF;
  for (const x of bytes) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c ^ 0xFFFF;
}

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const u16 = (b, at) => b[at] | b[at + 1] << 8;

/* One 12-byte entry -> { time, initials, car, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[7] !== 0) return null;
  const time = u32(bytes, 0), car = u32(bytes, 8);
  const initials = String.fromCharCode(bytes[4], bytes[5], bytes[6]);
  if (!INITIAL.test(initials) || time === 0 || car > 3) return null;
  return { time, initials, car, hex: bytesToHex(bytes) };
}

/* True when every section of a copy carries its own CRC, as the game checks it. */
export function copyValid(backup, copy) {
  if (u16(backup, copy + 2) !== COPY_SIZE) return false;
  return SECTIONS.every(([crcAt, from, to]) =>
    u16(backup, copy + crcAt) === crc16(backup.subarray(copy + from, copy + to)));
}

const tableAt = (copy, t) => copy + RECORDS_AT + t * RECORD_SIZE + RANKING_IN_RECORD;

/* The six tables, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? 0;
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

/* Writes the tables (best first, 16 each) into both copies and signs the section again. */
export function writeTables(backup, tables) {
  for (const copy of COPIES) {
    tables.forEach((table, t) => {
      for (let i = 0; i < ENTRIES && i < table.length; i++) {
        backup.set(hexToBytes(table[i].hex), tableAt(copy, t) + i * ENTRY_SIZE);
      }
    });
    const [crcAt, from, to] = SECTIONS[2];
    const c = crc16(backup.subarray(copy + from, copy + to));
    backup[copy + crcAt] = c & 0xFF;
    backup[copy + crcAt + 1] = c >> 8;
  }
  return backup;
}

/* m'ss"cc, the hundredths worked out from frames as the game shows them. */
export function formatTime(frames) {
  const cs = Math.floor((frames % 60) * 100 / 60), s = Math.floor(frames / 60) % 60;
  return `${Math.floor(frames / 3600)}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}

export function carName(car) { return CARS[car] || ''; }
