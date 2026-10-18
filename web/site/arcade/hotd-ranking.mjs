// The House of the Dead's ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.hotd). Read out of the
// program the container has (MAME's hotdo, the first release; m2-hle2's hotd profile
// loads it when Revision A is missing) (Pinboard #1091):
//
// The game keeps two copies of a 0x7C-byte block, at 0x100 and 0x17C. A copy is a u32 LE
// checksum, then 10 entries of 12 bytes, best first. The checksum is the program's CRC
// (0x1e420; the same routine Dynamite Cop has, see crc16 below) over the 0x78 bytes of
// entries; the game compares the whole u32 with it, so its upper half is 0. At boot
// (0x1dd00): a good first copy is copied over a second that differs; a bad first copy
// is replaced by a good second; two bad copies are wiped back to the factory ranking
// (ROM 0xaa490). After a name entry the game signs the first copy and copies it (0x1de90).
//   +0  3 chars initials, then a NUL (the name entry writes them in place)
//   +4  u8  the rank the game gave the play, shown as "RANK n+1" (0-7), then 3 zero bytes
//   +8  u32 LE  score: HIGHER wins (the factory TFF 38000 to KOB 14000)
//
// The service sorts by `time` ascending, so an entry's `time` is 0xFFFFFFFF - score
// (the best score sorts first); `score` holds the score itself, and formatTime(time)
// shows the score.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'hotd';
export const NVRAM_KEY = 'm2hle2.nvram.hotd';
export const ENTRY_SIZE = 12;
export const ENTRIES = 10;
export const COPY_SIZE = 0x7C;
export const COPIES = [0x100, 0x100 + COPY_SIZE];
export const TABLES = ['Ranking'];

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 999999;              // the ranking shows five digits; leave a margin
const INITIAL = /^[\x20-\x5a]{3}$/;    // what the ranking draws: space to Z

/* The program's 16-bit CRC (poly 0x1021 in a 32-bit register, seeded 0xDEBDEB00). */
export function crc16(bytes) {
  let g = 0xDEBDEB00;
  const shift = () => { g = g > 0x7FFFFFFF ? ((g << 1) + 0x10210000) >>> 0 : (g << 1) >>> 0; };
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) >>> 0) + x;
    for (let i = 0; i < 8; i++) shift();
  }
  for (let i = 0; i < 8; i++) shift();
  return g >>> 16;
}

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* One 12-byte entry -> { time, score, initials, grade, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[3] !== 0) return null;
  if (bytes[5] || bytes[6] || bytes[7] || bytes[4] > 7) return null;
  const initials = String.fromCharCode(bytes[0], bytes[1], bytes[2]);
  const score = u32(bytes, 8);
  if (!INITIAL.test(initials) || score === 0 || score > MAX_SCORE) return null;
  return { time: KEY - score, score, initials, grade: bytes[4], hex: bytesToHex(bytes) };
}

/* True when a copy carries its own checksum, as the game checks it. */
export function copyValid(backup, copy) {
  return u32(backup, copy) === crc16(backup.subarray(copy + 4, copy + COPY_SIZE));
}

/* The table, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  const out = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = copy + 4 + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the table (best first, 10) into both copies and signs each again. */
export function writeTables(backup, tables) {
  const table = tables[0] || [];
  for (const copy of COPIES) {
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      backup.set(hexToBytes(table[i].hex), copy + 4 + i * ENTRY_SIZE);
    }
    const c = crc16(backup.subarray(copy + 4, copy + COPY_SIZE));
    backup.set([c & 0xFF, c >> 8, 0, 0], copy);
  }
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

export function detail(entry) { return entry && entry.grade !== undefined ? `RANK ${entry.grade + 1}` : ''; }
