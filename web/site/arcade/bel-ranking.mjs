// Behind Enemy Lines' ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.bel). Read out of the bel
// program, the only set there is (Pinboard #1911).
//
// The game uses 0x3430 .. 0x3FFF. The boot (0x12f0) compares "S-RAM CHECK OK !!!" and
// its NUL (ROM 0x7dc80) with 0x3B00; a board without it is cleared (all 16 KB to zero,
// the string back) and given the factory rankings (0x51770; the test menu's resets call
// it too). No checksum covers the rankings; 0x3E00 and 0x3F00 are the settings and
// bookkeeping (0x12170 .. 0x127d0, which also use the words at 0x3EFE and 0x3FFE,
// likely their own sums); this module never writes there.
//
// Three tables are laid out, from descriptors of {slot, table, factory initials} in the
// data ROM (0x2976200): 12 entries of 24 bytes at 0x3440, 0x3680 and 0x38C0, and at
// 0x3430 + 4 * n a pointer to the entry the last play took in table n. The game picks the
// table with the byte at RAM 0x5a17c0, and every store to it in this program stores 2:
// only the third, at 0x38C0, is ever played or shown. The first two keep their factory
// entries (1600000 .. 1050000, EPL YU. RCH ...) and are left alone here.
//
// After a play (0x5ab80), for each of the two players, a score above the 12th's replaces
// it (score, play time, initials "   ") and 0x5aac0 bubbles it up past every LOWER score,
// moving +4 .. +0x17 and leaving each slot's rank where it is; the name entry then writes
// the initials in place. 0x51980 shows the table, six to a page, the score in 7 digits.
//   +0    u32 LE  the rank, 1 .. 12 (stays with the slot)
//   +4    u32 LE  score, HIGHER wins (the factory 750000 down to 475000 by 25000)
//   +8    u32     0, +0xC u32 0 (moved with the entry, written by nothing but the fill)
//   +0x10 u32 LE  the play's time in frames (RAM 0x5fa138, counted up in play); the
//                 factory 0x4B00 (5'20")
//   +0x14 3 ASCII initials and a NUL (factory: EPL SMW DEN CEB SJB E.H BLT TAP BAM LGS
//                 HJW JWW)
//
// The service sorts by `time` ascending, so an entry's `time` is KEY - score. The rank
// is the slot's, not the entry's: an entry's `hex` carries it as 0 (so the same play
// ranked 3rd on one board and 4th on another is one entry), and writeTables puts each
// slot's rank back.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'bel';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 24;
export const ENTRIES = 12;
export const RANKING_AT = 0x38C0;
export const MAGIC_AT = 0x3B00;
export const MAGIC = 'S-RAM CHECK OK !!!';   // and a NUL
export const TABLES = ['Ranking'];
export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)

const KEY = 0xFFFFFFFF;
const MAX_SCORE = 9999999;     // seven digits on the ranking
const INITIAL = /^[\x20-\x7e]{3}$/;
const MAGIC_BYTES = [...MAGIC].map((c) => c.charCodeAt(0)).concat(0);

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* True when the board carries the string the game checks at boot. */
export function magicValid(backup) {
  return MAGIC_BYTES.every((x, i) => backup[MAGIC_AT + i] === x);
}

/* One 24-byte entry -> { time, score, initials, frames, hex } (hex with the rank 0), or
 * null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const rank = u32(bytes, 0), score = u32(bytes, 4);
  if (rank > ENTRIES || score === 0 || score > MAX_SCORE || bytes[0x17] !== 0) return null;
  const initials = String.fromCharCode(bytes[0x14], bytes[0x15], bytes[0x16]);
  if (!INITIAL.test(initials)) return null;
  const b = bytes.slice();
  b.fill(0, 0, 4);
  return { time: KEY - score, score, initials, frames: u32(bytes, 0x10), hex: bytesToHex(b) };
}

/* The table (a board without the string is one the game would wipe: no entries). */
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

/* Writes the table (best first, 12 at most), each slot's rank, and the string; there is
 * no checksum to sign. */
export function writeTables(backup, tables) {
  const table = tables[0] || [];
  for (let i = 0; i < ENTRIES && i < table.length; i++) {
    const at = RANKING_AT + i * ENTRY_SIZE;
    backup.set(hexToBytes(table[i].hex).subarray(4), at + 4);
    backup.set([i + 1, 0, 0, 0], at);
  }
  backup.set(MAGIC_BYTES, MAGIC_AT);
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }
