// Zero Gunner's score ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.zerogun). Read out of the
// zerogun program (Export, Model 2B) and checked against zerogunj (Japan, Model 2B) and
// zeroguna (Export, Model 2A) (Pinboard #1911): the three use the same backup addresses,
// and their factory boards are the same bytes. Code addresses are zerogun's; zerogunj's
// are the same up to the factory table, zeroguna's are a little lower (noted where so).
// The backup RAM is at 0x01D00000 in the i960's space.
//
// 0x00..0x07 a u64 play-time counter, outside any checksum. Then two copies of one block:
//   A  u16 LE checksum at 0x08 over 0x0A..0xA3 (0x9A bytes)
//   B  u16 LE checksum at 0xA4 over 0xA6..0x13F
// The checksum (0x45500; zeroguna 0x45488) is the CRC Sega's boards use (g = 0xDEBDEB00;
// for each byte, its low 8 bits become the byte, then 8 shifts: g = 2g, plus 0x10210000
// when the top bit was set; g >> 16), but WITHOUT the 8 shifts more at the end that House
// of the Dead's has. The game compares the low 16 bits (ldos). The boot (0x42aa0) keeps A
// if it checks out (and copies it over B, 0x42b70 / 0x42c30, when B's stored sum is not
// A's); else B if it does (copied over A, 0x42cc0, then both signed); else it clears
// 0x00..0x13F (0x42be0) and writes the factory ranking (0x4b8a0, from ROM 0x4b840;
// zerogunj 0x4b890, zeroguna 0x4b740). Right after that check (0x7ec) it counts the boot
// in A's first u16 (0x0A) WITHOUT signing A again: so the factory board's copy A fails
// its checksum and B is the one that counts. readTables uses the first copy that checks
// out, as the game does.
//
// In each copy (A at 0x0A, B at 0xA6), at +0x3A (0x44 / 0xE0) the ranking, 8 entries of
// 12 bytes, best first:
//   +0  3 chars initials, then a NUL
//   +4  u16 LE  the player's ship, 0..2 (from 0x512660; not on the ranking screen)
//   +6  u16 LE  the stage reached (0x512946 + 1): the ranking's STAGE column
//   +8  u32 LE  score: HIGHER wins (the factory AAA 20000 down to HHH 13000, stage 1).
//               The insertion (0x4b990) puts the score before the first entry it equals or
//               beats (unsigned) and slides the rest down; the initials start as "A  ".
// The name entry's letters (ROM 0x4b720) are A-Z, 0-9, '-', '.' and a space; it turns "SEX"
// into " \x01 " (0x4b748 -> 0x4b750), shown here as " * ".
// Not seen in a play here (a shooter: an entry the game wrote itself was not practical to
// get in MAME); the layout is the program's and the factory board's.
//
// The service sorts by `time` ascending, so an entry's `time` is 0xFFFFFFFF - score
// (the best score sorts first); `score` holds the score itself, and formatTime(time)
// shows the score. zerogunaj (Japan, Model 2A) is not in the container: not checked.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'zerogun';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 12;
export const ENTRIES = 8;
/* Each copy: where its checksum is, and the bytes it covers. */
export const COPIES = [{ sum: 0x08, from: 0x0A, to: 0xA4 }, { sum: 0xA4, from: 0xA6, to: 0x140 }];
export const RANKING_OFFSET = 0x3A;   // from the copy's start: 0x44 and 0xE0
export const TABLES = ['Score ranking'];
/* The profiles whose board is this one byte for byte. */
export const SETS = { zerogun: 'zerogun', zerogunj: 'zerogunj', zeroguna: 'zeroguna' };

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 99999999;    // a bound of ours: the game keeps a u32
const MAX_STAGE = 99;
const INITIAL = /^[\x01\x20-\x5a]{3}$/;

const u16 = (b, at) => b[at] | b[at + 1] << 8;
const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* The game's checksum (0x45500) over some bytes. */
export function crc16(bytes) {
  let g = 0xDEBDEB00;
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) >>> 0) + x;
    for (let i = 0; i < 8; i++) g = g > 0x7FFFFFFF ? ((g << 1) + 0x10210000) >>> 0 : (g << 1) >>> 0;
  }
  return g >>> 16;
}

/* One 12-byte entry -> { time, score, initials, ship, stage, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[3] !== 0) return null;
  const raw = String.fromCharCode(bytes[0], bytes[1], bytes[2]);
  const ship = u16(bytes, 4), stage = u16(bytes, 6), score = u32(bytes, 8);
  if (!INITIAL.test(raw) || ship > 2 || stage < 1 || stage > MAX_STAGE) return null;
  if (score === 0 || score > MAX_SCORE) return null;
  return { time: KEY - score, score, initials: raw.replace(/\x01/g, '*'), ship, stage,
    hex: bytesToHex(bytes) };
}

/* True when a copy carries its own checksum, as the game checks it. */
export function copyValid(backup, copy) {
  return u16(backup, copy.sum) === crc16(backup.subarray(copy.from, copy.to));
}

/* The table, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[1];
  const out = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = copy.from + RANKING_OFFSET + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the table (best first, 8) into both copies and signs each again. */
export function writeTables(backup, tables) {
  const table = tables[0] || [];
  for (const copy of COPIES) {
    let changed = false;
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      const at = copy.from + RANKING_OFFSET + i * ENTRY_SIZE, bytes = hexToBytes(table[i].hex);
      if (bytes.some((x, k) => backup[at + k] !== x)) { backup.set(bytes, at); changed = true; }
    }
    /* A copy whose ranking is as it was keeps its sum: the factory board's unsigned A
     * (the boot counter) stays as the game left it, and B still counts. */
    if (!changed) continue;
    const c = crc16(backup.subarray(copy.from, copy.to));
    backup.set([c & 0xFF, c >> 8], copy.sum);
  }
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

/* The ranking's STAGE column. */
export function detail(entry) { return entry && entry.stage ? `STAGE ${entry.stage}` : ''; }
