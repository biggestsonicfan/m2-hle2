// Dynamite Cop's best score, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.dynamcopb, the default
// profile's set). Read out of the dynamcopb program (Pinboard #1091):
//
// The game keeps NO ranking with initials in backup RAM. Its ranking screens (attract
// states RANKING / RANK_NM) are kept in work RAM and start over at power-on. What
// it keeps is the HI-SCORE shown at the top of the screen: one u32, with no initials.
//
// Two copies of a 0x4FC-byte block, at 0x008 and 0x504. A copy is a u16 LE CRC (the
// program's routine at 0x12ff0, see crc16 below) over the rest of the copy: [0x00A,
// 0x504) and [0x506, 0xA00). At boot (0x2ea0): a good first copy is copied over a second
// whose CRC differs; a bad first copy is replaced by a good second; two bad copies (or a
// u32 above 0x1388 at 0x500) wipe all 16 KB to zero and write the factory block again.
// The block is bookkeeping (play counts, play-time histograms, the test menu's) and:
//   +0x4F4 (0x4FC, and 0x9F8 in the second copy)  u32 LE  hi-score; the game stores a
//          player's score there when it beats it (0x2eaa4, 0x39234) and signs again.
// Bytes 0x000-0x007 (a running counter) are outside both copies.
//
// HIGHER wins. The service sorts by `time` ascending, so the entry's `time` is
// 0xFFFFFFFF - score; `score` holds the score, formatTime(time) shows the score.
// `initials` is '' (the game keeps none).

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'dynamcopb';
export const NVRAM_KEY = 'm2hle2.nvram.dynamcopb';
export const ENTRY_SIZE = 4;
export const ENTRIES = 1;
export const COPY_SIZE = 0x4FC;
export const COPIES = [0x008, 0x008 + COPY_SIZE];
export const HISCORE_IN_COPY = 0x4F4;
export const TABLES = ['Hi-score'];

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 99999999;   // not read from the program: a bound that keeps junk out

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
const u16 = (b, at) => b[at] | b[at + 1] << 8;

/* The 4-byte hi-score -> { time, score, initials: '', hex }, or null (0, junk). */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const score = u32(bytes, 0);
  if (score === 0 || score > MAX_SCORE) return null;
  return { time: KEY - score, score, initials: '', hex: bytesToHex(bytes) };
}

/* True when a copy carries its own CRC, as the game checks it. */
export function copyValid(backup, copy) {
  return u16(backup, copy) === crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
}

/* The one table, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  const at = copy + HISCORE_IN_COPY;
  const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
  return [e ? [e] : []];
}

/* Writes the best score into both copies and signs each again. */
export function writeTables(backup, tables) {
  const best = (tables[0] || [])[0];
  for (const copy of COPIES) {
    if (best) backup.set(hexToBytes(best.hex), copy + HISCORE_IN_COPY);
    const c = crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
    backup[copy] = c & 0xFF;
    backup[copy + 1] = c >> 8;
  }
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

export function detail() { return ''; }
