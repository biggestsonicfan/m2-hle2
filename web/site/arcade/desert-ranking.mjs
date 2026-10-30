// Desert Tank's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.desert). Read out of the
// program the container has (MAME's desert, its only set; m2-hle2's one profile)
// (Pinboard #1911). The backup RAM is at 0x01D00000 in the i960's space; code addresses
// are the program ROM's, 0x28xxxxx the data ROM's.
//
// No checksum covers the rankings. At 0x3B00 the text "S-RAM CHECK OK !!!"; the boot
// (0x300c0) compares it with the program's (ROM 0x48790) and, when it differs, clears all
// 16 KB, writes the text again and fills the factory rankings (0xc6e0). The only other
// caller of the fill (0x3a778) also clears the bookkeeping (0x3ae40): a backup clear.
// 0x3E00 and 0x3F00 are two copies of the
// bookkeeping, each with its own checksum at +0xFE over its first 0xF0 bytes (0x3ade0,
// 0x3ae70); they do not reach the rankings.
//
// Three tables of 12 entries of 24 bytes, one per mode (0x52106f), from a descriptor in
// the data ROM (0x2891dd0: per mode, the slot holding the address of the last entry
// written, the table, the factory initials):
//   mode 0  slot 0x3430, table 0x3440   nothing seen shows it: left alone here
//   mode 1  slot 0x3434, table 0x3680   "VICTORS - DESERT MISSION" (the Beginner mission)
//   mode 2  slot 0x3438, table 0x38C0   "VICTORS - CANYON MISSION" (the Expert mission)
// An entry, best first:
//   +0   u32 LE  the place, 1..12: it stays with the slot (the insertion moves +4..+0x17)
//   +4   u32 LE  score: HIGHER wins (signed compare). The factory Desert runs LGS 1200000
//               down to JSL 650000 by 50000, Canyon ESD 1600000 down to CAM 1050000.
//   +8   u32 LE  a byte from 0x5211a3 (0 in the factory entries and in the play below;
//               what it is was not found)
//   +0xC u32     0
//   +0x10 u32 LE a counter from 0x52103c that runs during the play, one a 2 frames (the
//               factory entries have 9600); not on the ranking screen
//   +0x14 3 chars initials, then a NUL
// At the end of a play (0xa8c0) a score at least the 12th's goes into the 12th slot with
// initials "   ", bubbles up (0xa7f0), and the slot's address is put in the mode's slot
// word, for the name entry (letters A-Z, '.', a space) to write the initials through.
// Seen in MAME (test/fixtures/desert-played-backup1): a Desert play scoring 777777 whose
// name entry ran out went in 10th with "   ", +0x10 = 2560, and 0x3434 = 0x01D03758.
//
// writeTables writes +4..+0x17 of each entry and keeps each slot's place number, and
// leaves the slot words alone (the game points them at a slot of the table either way).
// The service sorts by `time` ascending, so an entry's `time` is 0xFFFFFFFF - score
// (the best score sorts first); `score` holds the score itself, and formatTime(time)
// shows the score.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'desert';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 24;
export const ENTRIES = 12;
export const TABLE_OFFSETS = [0x3680, 0x38C0];
export const TABLES = ['Desert mission', 'Canyon mission'];
export const MAGIC_AT = 0x3B00;

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 99999999;    // a bound of ours: the game keeps a signed u32
const MAGIC = [...'S-RAM CHECK OK !!!\0'].map((c) => c.charCodeAt(0));
const INITIAL = /^[\x20-\x7e]{3}$/;

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* One 24-byte entry -> { time, score, initials, place, flag, ticks, hex }, or null if it
 * is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[0x17] !== 0) return null;
  const place = u32(bytes, 0), score = u32(bytes, 4), flag = u32(bytes, 8);
  const initials = String.fromCharCode(bytes[0x14], bytes[0x15], bytes[0x16]);
  if (place < 1 || place > ENTRIES || flag > 0xFF || u32(bytes, 0xC) !== 0) return null;
  if (score === 0 || score > MAX_SCORE || !INITIAL.test(initials)) return null;
  return { time: KEY - score, score, initials, place, flag, ticks: u32(bytes, 0x10),
    hex: bytesToHex(bytes) };
}

/* True when the board carries the text the game checks at boot. */
export function magicValid(backup) {
  return MAGIC.every((x, i) => backup[MAGIC_AT + i] === x);
}

/* The two tables (a board without the text is one the game wipes: no entries). */
export function readTables(backup) {
  return TABLE_OFFSETS.map((base) => {
    const out = [];
    if (!magicValid(backup)) return out;
    for (let i = 0; i < ENTRIES; i++) {
      const at = base + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes each table (best first, 12) behind the slots' own place numbers, and the text;
 * there is no checksum to sign. */
export function writeTables(backup, tables) {
  TABLE_OFFSETS.forEach((base, t) => {
    const table = tables[t] || [];
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      const at = base + i * ENTRY_SIZE;
      backup.set(hexToBytes(table[i].hex).subarray(4), at + 4);
      backup.set([i + 1, 0, 0, 0], at);
    }
  });
  backup.set(MAGIC, MAGIC_AT);
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

export function detail() { return ''; }
