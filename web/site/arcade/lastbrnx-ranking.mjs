// Last Bronx's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.lastbrnx). Read out of the
// lastbrnx (Export, Revision A) program (Pinboard #1559). Code addresses are the program
// ROM's; 0x5xxxxx are its data, copied to RAM (RAM 0x500000 = ROM 0xc9100).
//
// The board starts with the header "ACT7 BackUp Data Header 96/06/04 21:51" at 0. The boot
// (0x3e570) compares it with the program's copy (0x3e4d0); a board without it is cleared
// (0x3e500: all 16 KB to zero, the header back) and filled with the factory tables
// (0x422c0). No checksum covers anything; 0x40..0x9f and 0x38c.. are bookkeeping (play
// counts and times, 0x3e7c0). 0x422c0 also runs when the test menu's game settings change
// (0x36b1c), which puts the factory tables back. The ranking viewer (0x41f30) pages
// through the three kinds below.
//
// A character is named in the backup by its 8-byte name, from the table at ROM 0xcd090
// (9-byte slots): 0 YOKO, 1 JOE, 2 NAGI, 3 KUROSAWA (eight letters, no NUL), 4 YUSAKU,
// 5 TOMMY, 6 LISA, 7 ZAIMOKU, 8 REDEYE. The per-character tables are in that order.
//
//  TIMEATTACK RANKING, 15 entries of 16 bytes at 0xA4, lower time wins (0x2c8b0 puts the
//  clear time in before the first slot it beats and shifts the rest; 0x2ce30 copies the 15
//  back after the name entry; 0x41970 shows them, five a page):
//   +0   u32 LE  time in frames (1/60 s), shown m'ss"cc: t/3600, t/60 % 60, t*100/60 % 100;
//                the factory times run from 324000 (90'00"00) by 1800
//   +4   3 chars initials, then a NUL (factory ones are staff names, lower case too)
//   +8   8 bytes the character's name as above, NUL padded
//
//  Best time for each character, 9 slots of 8 bytes at 0x194, by character (0x2d10c
//  writes the slot when the clear time beats it; 0x41b50 shows them): u32 time, 3 initials
//  and a NUL. The service sees a slot as a 16-byte entry like a ranking one (the slot's
//  character's name at +8), so `detail` names the character; writeTables puts each
//  character's best in its own slot and leaves the slot of a character with no entry
//  alone.
//
//  SURVIVAL RANKING, one table per character, 5 entries of 8 bytes, at 0x224 + 0x28 * the
//  character (0x2ab34 inserts the score, 0x2b5cc copies the five back; 0x41d90 shows
//  them): u32 score (higher wins, at most 9999999, the factory 5000 down to 1000), 3
//  initials and a NUL. The service sees each as a 16-byte entry with the table's
//  character's name at +8, like the slots above, and keeps `time` = 2^32 + 9999999 -
//  score, above every time, so the lowest key is the best score; formatTime tells the two
//  apart by that range.
//
// 0x1dc: 9 x 8 bytes (u32 0 and the characters' short names YKO, JOE ... RED) the factory
// fill writes and nothing else in the program reads; left alone.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'lastbrnx';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 16;
export const ENTRIES = 15;
export const HEADER = 'ACT7 BackUp Data Header 96/06/04 21:51';

export const RANKING_AT = 0xA4;
export const CHARACTER_BEST_AT = 0x194;
export const SURVIVAL_AT = 0x224;
export const SURVIVAL_ENTRIES = 5;
const SLOT_SIZE = 8, SURVIVAL_SIZE = SURVIVAL_ENTRIES * SLOT_SIZE;

/* The names the backup keeps (ROM 0xcd090) and the ones shown here. */
const NAMES = ['YOKO', 'JOE', 'NAGI', 'KUROSAWA', 'YUSAKU', 'TOMMY', 'LISA', 'ZAIMOKU', 'REDEYE'];
const CHARACTERS = ['Yoko', 'Joe', 'Nagi', 'Kurosawa', 'Yusaku', 'Tommy', 'Lisa', 'Zaimoku',
  'Red Eye'];

export const TABLES = ['Time attack', 'Best time for each character',
  ...CHARACTERS.map((c) => `Survival: ${c}`)];
const SURVIVAL_TABLE = 2;   // tables 2..10: survival, one per character in NAMES order
/* How many each table keeps, and which rank by points (games.mjs). The best-time table
 * keeps more than its 9 slots through the merge, so that a character's new best is not cut
 * by the others' before writeTables picks each character's own. */
export const TABLE_ENTRIES = [ENTRIES, ENTRIES, ...CHARACTERS.map(() => SURVIVAL_ENTRIES)];
export const TABLE_SCORE = TABLES.map((_, t) => t >= SURVIVAL_TABLE);

const MAX_TIME = 360000;        // 100'00"00: the ranking shows two digits of minutes
const MAX_SCORE = 9999999;      // the game stops the survival score there (0x2ab24)
const SCORE_KEY = 2 ** 32;
const INITIAL = /^[\x20-\x7e]{3}$/;

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* The 8 bytes a character's name takes in an entry. */
function nameBytes(c) {
  const b = new Uint8Array(8);
  for (let i = 0; i < NAMES[c].length; i++) b[i] = NAMES[c].charCodeAt(i);
  return b;
}
const NAME_HEX = NAMES.map((_, c) => bytesToHex(nameBytes(c)));

/* One 16-byte entry of table `table` (0 if not given) -> { time, initials, character, hex }
 * (and `score` in a survival table), or null if it is not one. */
export function parseEntry(bytes, table = 0) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[7] !== 0) return null;
  if (!Number.isInteger(table) || table < 0 || table >= TABLES.length) return null;
  const character = NAME_HEX.indexOf(bytesToHex(bytes.subarray(8, 16)));
  const initials = String.fromCharCode(bytes[4], bytes[5], bytes[6]);
  if (character < 0 || !INITIAL.test(initials)) return null;
  return entryValue(u32(bytes, 0), table, { initials, character, hex: bytesToHex(bytes) });
}

/* An entry's first word in table `table`: a time, or a survival table's score
 * (its own character's only), sorted as a time after every real one. Null if
 * it is out of range. */
function entryValue(value, table, e) {
  const { initials, character, hex } = e;
  if (table >= SURVIVAL_TABLE) {
    if (character !== table - SURVIVAL_TABLE || value === 0 || value > MAX_SCORE) return null;
    return { time: SCORE_KEY + MAX_SCORE - value, score: value, initials, character, hex };
  }
  if (value === 0 || value >= MAX_TIME) return null;
  return { time: value, initials, character, hex };
}

/* An 8-byte slot of character c as the 16-byte entry the service keeps, or null. */
function slotEntry(backup, at, c, table) {
  const b = new Uint8Array(ENTRY_SIZE);
  b.set(backup.subarray(at, at + SLOT_SIZE), 0);
  b.set(nameBytes(c), 8);
  return parseEntry(b, table);
}

export function readTables(backup) {
  const ranking = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = RANKING_AT + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE), 0);
    if (e) ranking.push(e);
  }
  const best = [];
  for (let c = 0; c < NAMES.length; c++) {
    const e = slotEntry(backup, CHARACTER_BEST_AT + c * SLOT_SIZE, c, 1);
    if (e) best.push(e);
  }
  best.sort((a, b) => a.time - b.time);
  const survival = NAMES.map((_, c) => {
    const out = [];
    for (let i = 0; i < SURVIVAL_ENTRIES; i++) {
      const e = slotEntry(backup, SURVIVAL_AT + c * SURVIVAL_SIZE + i * SLOT_SIZE, c,
        SURVIVAL_TABLE + c);
      if (e) out.push(e);
    }
    return out;
  });
  return [ranking, best, ...survival];
}

/* Writes the time attack ranking (best first, 15 at most), each character's best time
 * into its slot, and each survival table (best first, 5 at most). No checksum to sign. */
export function writeTables(backup, tables) {
  const [ranking = [], best = []] = tables;
  for (let i = 0; i < ENTRIES && i < ranking.length; i++) {
    backup.set(hexToBytes(ranking[i].hex), RANKING_AT + i * ENTRY_SIZE);
  }
  const top = new Map();
  for (const e of best) {
    const p = parseEntry(hexToBytes(e.hex), 1);
    if (!p) continue;
    const prev = top.get(p.character);
    if (!prev || p.time < prev.time) top.set(p.character, p);
  }
  for (const [c, p] of top) {
    backup.set(hexToBytes(p.hex).subarray(0, SLOT_SIZE), CHARACTER_BEST_AT + c * SLOT_SIZE);
  }
  for (let c = 0; c < NAMES.length; c++) {
    const table = (tables[SURVIVAL_TABLE + c] || [])
      .map((e) => parseEntry(hexToBytes(e.hex), SURVIVAL_TABLE + c)).filter(Boolean);
    for (let i = 0; i < SURVIVAL_ENTRIES && i < table.length; i++) {
      backup.set(hexToBytes(table[i].hex).subarray(0, SLOT_SIZE),
        SURVIVAL_AT + c * SURVIVAL_SIZE + i * SLOT_SIZE);
    }
  }
  return backup;
}

/* A time in frames as the game shows it (m'ss"cc), or a survival score from its key. */
export function formatTime(t) {
  if (t >= SCORE_KEY) return String(SCORE_KEY + MAX_SCORE - t);
  const cs = Math.floor(t * 100 / 60) % 100, s = Math.floor(t / 60) % 60, m = Math.floor(t / 3600);
  return `${m}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}

export function characterName(c) { return CHARACTERS[c] || ''; }

export function detail(entry) { return entry ? characterName(entry.character) : ''; }
