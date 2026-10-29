// Virtua Cop's ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.vcop). Read out of the
// program the container has (MAME's vcop, Revision B; m2-hle2's one profile) (Pinboard #1559).
// The backup RAM is at 0x01D00000 in the i960's space.
//
// One copy, no checksum. At 0x200 the magic "sega\0" (the 3 bytes after it are left as
// they were); at boot (0x11c80) the game compares it with "sega" and, when it differs,
// shows "memory destroyed" and writes the factory ranking (0x11cc0: initials from data
// ROM 0x2814a60, scores from 0x2814b10) and the magic again. Then 10 entries of 16 bytes
// at 0x208, best first:
//   +0  u32 LE  score: HIGHER wins (the factory ITA 100000 to NOO 10000). The game
//               rewrites the factory ranking if one is negative or above 9999999 (0x114a0).
//   +4, +8, +12  u32 LE  one initial each, a code 0-29: A-Z are 0-25, 26 '.', 27 and 28
//               the name entry's BS and END marks, 29 a space (glyph table 0x2814a90).
//               Above 29 the game rewrites the factory ranking (0x119dc).
// At the end of a play (0x67680) the game slides the entries below the player's score down
// one slot and stores the score (the slot keeps the initials it had until the name entry
// writes the player's, 0x6aad4 / 0x6acc8 for players 1 / 2). Seen in MAME: a play of
// 200000 whose name entry ran out left the initials as three spaces (code 29); the play
// also wrote three u32 29 at 0x84C (outside the ranking; left alone here).
//
// The service sorts by `time` ascending, so an entry's `time` is 0xFFFFFFFF - score
// (the best score sorts first); `score` holds the score itself, and formatTime(time)
// shows the score. `initials` shows BS as '<' and END as '>'.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'vcop';
export const NVRAM_KEY = 'm2hle2.nvram.vcop';
export const ENTRY_SIZE = 16;
export const ENTRIES = 10;
export const MAGIC_AT = 0x200;
export const RANKING_AT = 0x208;
export const TABLES = ['Ranking'];

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 9999999;     // the game's own bound (0x114a0)
const MAGIC = [0x73, 0x65, 0x67, 0x61, 0x00];   // "sega\0"
export const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ.<> ';   // code n is CHARS[n]

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* One 16-byte entry -> { time, score, initials, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const score = u32(bytes, 0);
  if (score === 0 || score > MAX_SCORE) return null;
  let initials = '';
  for (let i = 1; i < 4; i++) {
    const c = u32(bytes, i * 4);
    if (c >= CHARS.length) return null;
    initials += CHARS[c];
  }
  return { time: KEY - score, score, initials, hex: bytesToHex(bytes) };
}

/* True when the board carries the magic the game checks at boot. */
export function magicValid(backup) {
  return MAGIC.every((x, i) => backup[MAGIC_AT + i] === x);
}

/* The table (a board without the magic is one the game would wipe: no entries). */
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
