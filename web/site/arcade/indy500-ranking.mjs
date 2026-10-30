// INDY 500's TOP 10 RANKINGs, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.indy500). Read out of the
// program the container has (epr-18254a/18255a, MAME's indy500to; m2-hle2's indy500
// profile loads it) (Pinboard #1911). The Deluxe set (indy500d, epr-18251a/18252a) keeps
// a different, shorter layout (8-byte entries after a float per course, copies of 0x130
// bytes at 0x08 and 0x138) and is not read here.
//
// The game keeps two copies of a 0x1FC-byte block, at 0x08 and 0x204. A copy is a u16 LE
// CRC over its other 0x1FA bytes. The CRC is the program's (0x25ca0): poly 0x1021 in a
// 32-bit register seeded 0xDEBDEC00, the upper half taken after the last byte (see crc16
// below). At boot (0x8f50): a good first copy whose CRC the second's does not match is
// signed again and copied over it (0x9000, 0x9190); a bad first copy is replaced by a good
// second (0x91d0); two bad copies are cleared (0x000-0x3FF) and filled with the factory
// tables from ROM 0x6da20 (0x9020). 0x00-0x07 (outside the copies) and 0x0A-0x3B are
// bookkeeping; 0x1A4-0x203 are floats per course (section records, 0x288xx) left alone.
//
// Three tables of 10 entries of 12 bytes, best first, at copy + 0x34 + 0x78 * course
// (0x3C, 0xB4, 0x12C in the first copy). The courses, matched with the attract screens by
// their factory times: 0 BAY SIDE STREET (197-206 s), 1 INDY 500 (the Speedway, 152-161 s),
// 2 HIGHLAND RACEWAY (153-162 s). The game reads them into RAM at boot (0x4ea10), inserts
// a finish (0x36dd8 -> 0x4e940: before the first entry it beats, a tie goes after) and
// writes the ten back in order (0x4ed20), then signs the copies.
//   +0  3 bytes initials, a code each: 0-9 the digits, 0x0A-0x23 A-Z, 0x24 '.', 0x25 '!',
//       0x26 '?', 0x27 a space (the name entry's blank). Codes from 0x28 up draw the time's
//       digits and marks: not a name. (Shown on the attract ranking from a poked board.)
//       The name entry swaps a few words for others (0x36d20, lists at 0x36a50/0x36a60).
//   +3  u8  0 MT (manual), 1 AT, shown beside the time
//   +4  u8  from 0x53c584 and +5 u8 from 0x5ebf4c: not shown; 0 in the factory tables
//   +6  2 zero bytes (never written)
//   +8  f32 LE  total time in seconds, shown mm'ss"mmm; lower wins
//
// The service sorts by `time`: here the time in milliseconds, rounded; `hex` keeps the
// float as the game wrote it. Checked in MAME: a board written as below boots with its
// entries on the ranking screen (the CRC holds).

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'indy500';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 12;
export const ENTRIES = 10;
export const COPY_SIZE = 0x1FC;
export const COPIES = [0x08, 0x08 + COPY_SIZE];
export const TABLES = ['Bay Side Street', 'Indy 500', 'Highland Raceway'];

const TABLES_IN_COPY = 0x34, TABLE_SIZE = ENTRIES * ENTRY_SIZE;
export const CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.!? ';
const MAX_SECONDS = 6000;       // the ranking shows two digits of minutes

/* The program's 16-bit CRC (0x25ca0). */
export function crc16(bytes) {
  let g = 0xDEBDEC00;
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) >>> 0) + x;
    for (let i = 0; i < 8; i++) g = g > 0x7FFFFFFF ? ((g << 1) + 0x10210000) >>> 0 : (g << 1) >>> 0;
  }
  return g >>> 16;
}

const u16 = (b, at) => b[at] | b[at + 1] << 8;

/* One 12-byte entry -> { time, seconds, initials, transmission, hex }, or null. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[6] || bytes[7] || bytes[3] > 1) return null;
  if (bytes[0] >= CHARS.length || bytes[1] >= CHARS.length || bytes[2] >= CHARS.length) return null;
  const seconds = new DataView(bytes.buffer, bytes.byteOffset, ENTRY_SIZE).getFloat32(8, true);
  if (!(seconds > 0 && seconds < MAX_SECONDS)) return null;
  const initials = CHARS[bytes[0]] + CHARS[bytes[1]] + CHARS[bytes[2]];
  return { time: Math.round(seconds * 1000), seconds, initials, transmission: bytes[3], hex: bytesToHex(bytes) };
}

/* True when a copy carries its own CRC, as the game checks it. */
export function copyValid(backup, copy) {
  return u16(backup, copy) === crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
}

const tableAt = (copy, t) => copy + TABLES_IN_COPY + t * TABLE_SIZE;

/* The three tables, from the copy the game would keep (the first that checks out). */
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

/* Writes the tables (best first, 10 each) into the first copy, signs it and copies it over
 * the second, as the game does. A bad first copy is first replaced by a good second. */
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
  backup.copyWithin(b, a, a + COPY_SIZE);
  return backup;
}

/* Milliseconds as the game shows a total time: mm'ss"mmm. */
export function formatTime(ms) {
  const m = Math.floor(ms / 60000), s = Math.floor(ms / 1000) % 60, f = ms % 1000;
  return `${String(m).padStart(2, '0')}'${String(s).padStart(2, '0')}"${String(f).padStart(3, '0')}`;
}

export function detail(entry) { return entry ? (entry.transmission ? 'AT' : 'MT') : ''; }
