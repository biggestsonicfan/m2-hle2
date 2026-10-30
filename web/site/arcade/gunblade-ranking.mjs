// Gunblade NY's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.gunblade). Read out of the
// program the container has (MAME's gunblade, Revision A, epr-18988a/18989a; the only set)
// (Pinboard #1911):
//
// The game keeps two copies of a 0xC20-byte block, at 0x000 and 0xC20. The check
// (0xe0d0) wants, in a copy:
//   +0x000 u16 CRC over +0x002, 0x186 bytes (settings)
//   +0x002 u16 0x0C20, the block's size
//   +0x18C u16 CRC over +0x190, 0x84 bytes (bookkeeping)
//   +0x218 u32 the low half a CRC over +0x21C to the end, 0xA04 bytes (the rankings); the
//          game reads it with ldos and masks it, so the upper half stays as it is (FFFF)
// The CRC (0xc4cb0, table at 0xc4810) is CRC-16/GENIBUS: poly 0x1021, MSB first, seeded
// 0xFFFF, the result inverted. At boot (0xe1c0) a good first copy is kept; a bad one is
// replaced by a good second; two bad copies are wiped (0xdff0), the rankings back to the
// factory ones (0xe3f0 -> 0xca5a0, ROM 0xca300/0xca3e0/0xca4c0). Every frame (0xe3c0) the
// game signs the first copy again and copies it over the second.
//
// Two groups of three tables, 10 entries of 20 bytes each, best first:
//   "TOP 10 GUNNERS"          at 0x42C: tables at +0, +0x194, +0x328
//   "MONTHLY TOP 5 GUNNERS"   at 0x824: the same (keeps 10, shows 5)
// The tables of a group: REGAIN THE U.N. H.Q. (mission 1, time), BIG APPLE LIBERATION
// (mission 2, time), SCORE ATTACK REMIX (score). After each table's entries (+0xC8) comes
// the play count and the 50 words of a histogram (0xca838), which are not touched here.
// An entry (LE):
//   +0  u32 a time in frames (missions: LOWER wins; shown as seconds, 30000 = "500.00")
//       or a score (score attack: HIGHER wins)
//   +4  u32 the player's record the entry came from (+0x4c of it); 0 in the factory ones
//   +8  u32 its age in days (the monthly group drops it past 29, 0xcac00; 0 in group A)
//   +12 u32 the grade shown beside a mission time: 3 = S, 2 = A, 1 = B, 0 = C
//   +16 3 chars initials, then a space ("MIY ", "M.S ", "!.! ")
// The rank search (0xcab38) puts a new entry under the ones it ties with; the insert is
// 0xcaa70/0xcaad0, the registration 0xca8d0.
//
// The service keeps the all-time group (TOP 10 GUNNERS); the monthly group, which the game
// ages and refills with the factory entries by itself, is left as it is. Not played in
// MAME for an entry the game wrote itself (the layout is from the program and the factory
// board), but a board written by writeTables (ROB 29000 frames, grade S, first) was booted
// in MAME: its attract showed "1st ROB S 482.80" above "MIY S 500.00", and the board it
// saved still checked out with ROB in it.
//
// The service sorts by `time` ascending, so a score attack entry's `time` is
// SCORE_KEY + MAX_SCORE - score (TABLE_SCORE); formatTime tells the two apart by it.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'gunblade';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 20;
export const ENTRIES = 10;
export const COPY_SIZE = 0xC20;
export const COPIES = [0, COPY_SIZE];
export const RANKING_AT = 0x42C;           // the all-time group, in a copy
export const MONTHLY_AT = 0x824;           // the monthly group (not written here)
export const TABLE_STEP = 0x194;           // from one table of a group to the next
const SIGNED_AT = 0x21C, SUM_AT = 0x218;

export const TABLES = ['Regain the U.N. H.Q.', 'Big Apple Liberation', 'Score Attack Remix'];
export const TABLE_ENTRIES = TABLES.map(() => ENTRIES);
export const TABLE_SCORE = [false, false, true];
const SCORE_TABLE = 2;

const MAX_TIME = 60000;         // 999.99 seconds: what three digits of seconds show
const MAX_SCORE = 9999999;      // a guess with margin: the factory best is 30000
const MAX_AGE = 29;
const SCORE_KEY = 2 ** 32;
const INITIAL = /^[\x20-\x7e]{3}$/;
const GRADES = ['C', 'B', 'A', 'S'];

const u16 = (b, at) => b[at] | b[at + 1] << 8;
const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* The program's CRC: CRC-16/GENIBUS (0x1021, MSB first, seed 0xFFFF, inverted). */
export function crc16(bytes) {
  let c = 0xFFFF;
  for (const x of bytes) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return ~c & 0xFFFF;
}

/* True when a copy passes the game's boot check (0xe0d0). */
export function copyValid(backup, copy) {
  const b = backup.subarray(copy, copy + COPY_SIZE);
  return u16(b, 2) === COPY_SIZE
    && u16(b, 0) === crc16(b.subarray(2, 0x188))
    && u16(b, 0x18C) === crc16(b.subarray(0x190, 0x214))
    && u16(b, SUM_AT) === crc16(b.subarray(SIGNED_AT, COPY_SIZE));
}

/* One 20-byte entry of table `table` -> { time, initials, grade, hex } (and `score` in
 * the score attack), or null if it is not one. */
export function parseEntry(bytes, table = 0) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  if (!Number.isInteger(table) || table < 0 || table >= TABLES.length) return null;
  const value = u32(bytes, 0), age = u32(bytes, 8), grade = u32(bytes, 12);
  const initials = String.fromCharCode(bytes[16], bytes[17], bytes[18]);
  if (bytes[19] !== 0x20 || !INITIAL.test(initials) || age > MAX_AGE || grade > 3) return null;
  const hex = bytesToHex(bytes);
  if (table === SCORE_TABLE) {
    if (value === 0 || value > MAX_SCORE) return null;
    return { time: SCORE_KEY + MAX_SCORE - value, score: value, initials, grade, table, hex };
  }
  if (value === 0 || value >= MAX_TIME) return null;
  return { time: value, initials, grade, table, hex };
}

const tableAt = (copy, t) => copy + RANKING_AT + t * TABLE_STEP;

/* The all-time tables, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  return TABLES.map((_, t) => {
    const out = [];
    for (let i = 0; i < ENTRIES; i++) {
      const at = tableAt(copy, t) + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE), t);
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes each table (best first, 10 at most) into both copies and signs the rankings'
 * CRC of each again (its upper half kept). */
export function writeTables(backup, tables) {
  for (const copy of COPIES) {
    TABLES.forEach((_, t) => {
      const table = (tables[t] || []).map((e) => parseEntry(hexToBytes(e.hex), t)).filter(Boolean);
      for (let i = 0; i < ENTRIES && i < table.length; i++) {
        backup.set(hexToBytes(table[i].hex), tableAt(copy, t) + i * ENTRY_SIZE);
      }
    });
    const c = crc16(backup.subarray(copy + SIGNED_AT, copy + COPY_SIZE));
    backup.set([c & 0xFF, c >> 8], copy + SUM_AT);
  }
  return backup;
}

/* A mission time in frames as seconds with two decimals, or a score from its key. */
export function formatTime(t) {
  if (t >= SCORE_KEY) return String(SCORE_KEY + MAX_SCORE - t);
  const cs = Math.floor(t * 100 / 60);
  return `${Math.floor(cs / 100)}.${String(cs % 100).padStart(2, '0')}`;
}

/* The grade the game shows beside a mission time. */
export function detail(entry) {
  if (!entry || entry.table === SCORE_TABLE || entry.grade === undefined) return '';
  return `RANK ${GRADES[entry.grade]}`;
}
