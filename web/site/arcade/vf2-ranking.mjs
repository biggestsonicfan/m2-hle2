// Virtua Fighter 2's best times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like ranking.mjs (Sonic the Fighters', which keeps
// its ranking the same way: Fighting Vipers and STF grew out of this program). m2-hle2's
// web build keeps the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.vf2). Read
// out of the vf2 (Version 2.1) program (Pinboard #1091).
//
// One table, written straight into backup RAM (not through the work copy at 0x599000),
// lower time wins, in hundredths of a second: the clear time of the whole arcade mode,
// which stops counting at 360000 = 60'00"00 (the factory names run from 50'00"00).
//
//  Ranking, 18 entries of 16 bytes at 0x3100, best first (0x53924 inserts, 0x53e40 puts
//  the initials in; the attract's RANKING pages show all 18, six a page):
//   +0   u32 LE  time
//   +4   3 chars initials, then a NUL ("   " until the name entry fills them)
//   +8   u16     the game's own (0x50006e), 0 in the factory entries
//   +A   s8      character: 0 Akira, 1 Jacky, 2 Sarah, 3 Kage, 4 Lau, 5 Jeffry, 6 Pai,
//                7 Wolf, 8 Shun, 9 Dural, 10 Lion (the names at 0x201ec34)
//   +B   u8      a ratio the game keeps (0x50008e*100/0x50008d), 0 in the factory
//   +C   u8      another (player +0x1234*100/+0x1230), 0 in the factory; +D..+F zero
//                (only time, initials and character are shown)
//
// No checksum covers the table. The boot (0x6dddc) checks the header at 0x3308 ("VIRTUA
// FIGHTER 2", version 24 at 0x3306) and two CRC-16s (table-driven, 0x9480): 0x3300 over
// 0x3320..0x332e and 0x3302 over the settings at 0x3340..0x335c. A bad one sends the game
// to its factory reset, which also puts the factory ranking back (0x5427c: the first 7
// names fixed, the other 11 drawn at random, so another emulator's factory board can hold
// other initials); nothing here touches those bytes.
//
// The four sets keep the ranking alike (the same code at other addresses), and vf2b's
// factory board is vf2's; but the header's version differs: 24 in vf2 and vf2b, 19 in
// vf2a, 18 in vf2o, and a board with another version is wiped at boot. So a board is
// shared by vf2 and vf2b only.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'vf2';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 16;
export const ENTRIES = 18;
export const TABLES = ['Ranking'];
export const RANKING_AT = 0x3100;
/* The profiles whose board is this one byte for byte (same header version). */
export const SETS = { vf2: 'vf2', vf2b: 'vf2b' };

const MAX_TIME = 0x60000;   // the game stops adding at 360000; one more round goes past it
const INITIAL = /^[\x20-\x5f]{3}$/;
const CHARACTERS = ['Akira', 'Jacky', 'Sarah', 'Kage', 'Lau', 'Jeffry', 'Pai', 'Wolf',
  'Shun', 'Dural', 'Lion'];

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* One 16-byte entry -> { time, initials, character, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[7] !== 0) return null;
  if (bytes[13] || bytes[14] || bytes[15]) return null;
  const time = u32(bytes, 0), character = bytes[10];
  const initials = String.fromCharCode(bytes[4], bytes[5], bytes[6]);
  if (!INITIAL.test(initials) || time === 0 || time >= MAX_TIME) return null;
  if (character >= CHARACTERS.length) return null;
  return { time, initials, character, hex: bytesToHex(bytes) };
}

export function readTables(backup) {
  const out = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = RANKING_AT + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the ranking (best first, 18 at most). No checksum to sign. */
export function writeTables(backup, tables) {
  const table = tables[0] || [];
  for (let i = 0; i < ENTRIES && i < table.length; i++) {
    backup.set(hexToBytes(table[i].hex), RANKING_AT + i * ENTRY_SIZE);
  }
  return backup;
}

/* m'ss"cc from hundredths, as the game shows them (minutes run past 59). */
export function formatTime(t) {
  const cs = t % 100, s = Math.floor(t / 100) % 60, m = Math.floor(t / 6000);
  return `${m}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}

export function characterName(c) { return CHARACTERS[c] || ''; }

export function detail(entry) { return entry ? characterName(entry.character) : ''; }
