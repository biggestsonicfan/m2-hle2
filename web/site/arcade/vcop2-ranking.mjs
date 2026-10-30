// Virtua Cop 2's ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.vcop2). Read out of the
// program the container has (MAME's vcop2, its only set; m2-hle2's one profile)
// (Pinboard #1911). The backup RAM is at 0x01D00000 in the i960's space; code addresses
// are the program ROM's.
//
// One copy, no checksum over the ranking (the settings at 0x00 and 0x80 are two copies of
// their own, left alone). 10 entries of 8 bytes at 0x400, best first, then at 0x450 the
// 16 bytes "SEGA VIRTUA COP2". At boot (0x3e034) the game compares only the first u32
// ("SEGA") with the program's (ROM 0xcc720); when it differs it copies the 0x60 bytes of
// ROM 0xcc6d0 (the factory ranking and the magic) over 0x400 (0x3d2c0). The test menu's
// BACKUP DATA CLEAR (0x55d18) does the same. An entry:
//   +0  u24 LE  score: HIGHER wins (the factory WAT 100000 down to YOS 55000, by 5000).
//               The end of a play (0x3d324) puts the score in before the first entry it
//               beats strictly, and slides the rest down.
//   +3  s8      the grade the play earned, 0..10 (0x3ce50 works it out of five of the
//               play's counts; -1 for a player who did not play). The ranking screen shows
//               it as a badge (picture table 0xcc628) numbered grade + 1 (seen in MAME's
//               attract: AOC's grade 5 shows "6"); the factory entries have 3 to 7, the
//               best scores the lowest, so a low badge looks like the better one.
//   +4  3 chars initials, then a NUL. The insertion zeroes them until the name entry
//               writes the player's; the ranking shows A-Z, 0-9 and '.' and skips the rest
//               (so a NUL shows as nothing; here as a space).
// Not seen in a play here (a light-gun game: an entry the game wrote itself was not
// practical to get in MAME); the layout is the program's and the factory board's.
//
// The service sorts by `time` ascending, so an entry's `time` is 0xFFFFFFFF - score
// (the best score sorts first); `score` holds the score itself, and formatTime(time)
// shows the score.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'vcop2';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 8;
export const ENTRIES = 10;
export const RANKING_AT = 0x400;
export const MAGIC_AT = 0x450;
export const TABLES = ['Ranking'];

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 0xFFFFFF;    // the field's 24 bits
const MAX_GRADE = 10;
const MAGIC = [...'SEGA VIRTUA COP2'].map((c) => c.charCodeAt(0));
const INITIAL = /^[\0 .0-9A-Z]{3}$/;

const u24 = (b, at) => b[at] | b[at + 1] << 8 | b[at + 2] << 16;

/* One 8-byte entry -> { time, score, initials, grade, hex }, or null if it is not one.
 * `grade` is 0..10, or -1 for a player who did not play. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[7] !== 0) return null;
  const score = u24(bytes, 0);
  const grade = bytes[3] === 0xFF ? -1 : bytes[3];
  const raw = String.fromCharCode(bytes[4], bytes[5], bytes[6]);
  if (score === 0 || score > MAX_SCORE || grade > MAX_GRADE || !INITIAL.test(raw)) return null;
  return { time: KEY - score, score, initials: raw.replace(/\0/g, ' '), grade,
    hex: bytesToHex(bytes) };
}

/* True when the board carries the magic the game checks at boot (its first u32). */
export function magicValid(backup) {
  return MAGIC.slice(0, 4).every((x, i) => backup[MAGIC_AT + i] === x);
}

/* The table (a board without the magic is one the game would refill: no entries). */
export function readTables(backup) {
  const out = [];
  if (!magicValid(backup)) return [out];
  for (let i = 0; i < ENTRIES; i++) {
    const at = RANKING_AT + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the table (best first, 10) and the magic; there is no checksum to sign. */
export function writeTables(backup, tables) {
  const table = tables[0] || [];
  for (let i = 0; i < ENTRIES && i < table.length; i++) {
    backup.set(hexToBytes(table[i].hex), RANKING_AT + i * ENTRY_SIZE);
  }
  backup.set(MAGIC, MAGIC_AT);
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

/* The badge the ranking screen shows beside the entry. */
export function detail(entry) {
  return entry && entry.grade >= 0 ? `RANK ${entry.grade + 1}` : '';
}
