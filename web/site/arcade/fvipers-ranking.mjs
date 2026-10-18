// Fighting Vipers' best times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like ranking.mjs (Sonic the Fighters', its successor,
// which keeps its ranking the same way). m2-hle2's web build keeps the 16 KB of backup RAM
// in localStorage as hex (m2hle2.nvram.fvipers). Read out of the fvipers (Rev D) program
// (Pinboard #1091); fvipersb (Rev B) has the same layout and the same factory board.
//
// Two tables, both written straight into backup RAM (not through the work copy at
// 0x599000), both lower time wins, times in hundredths of a second (the clear time of the
// whole arcade mode, which stops counting at 360000 = 60'00"00; the factory names run from
// 50'00"00):
//
//  Ranking, 15 entries of 16 bytes at 0x3100, best first (0x50404 inserts, 0x51100 puts
//  the initials in; the attract's RANKING pages show all 15):
//   +0   u32 LE  time
//   +4   3 chars initials, then a NUL ("   " until the name entry fills them)
//   +8   u16     the game's own (0x50006e), 0 in the factory entries
//   +A   s8      character: 0 Jane, 1 Grace, 2 Honey, 3 Tokio, 4 Bahn, 5 Picky, 6 Raxel,
//                7 Sanman, 8 Mahler, 9 B.M. (the names at 0x6ce6e8a)
//   +B   u8      a ratio the game keeps (0x50008e*100/0x50008d), 0 in the factory
//   +C   u8      another (player +0x1234*100/+0x1230), 0 in the factory; +D..+F zero
//                (only time, initials and character are shown)
//
//  Best time for each character, 8 slots of 8 bytes at 0x3280, one per character 0-7 in
//  the order above (the game ANDs the character with 7): u32 time, 3 initials and a NUL.
//  An unset slot is time 0xFFFFFFFF, initials "   ". The service sees each slot as a
//  16-byte entry like a Ranking one (time, initials, the character at +A, the rest 0), so
//  `detail` names the character; writeTables puts each character's best in its own slot
//  and leaves the slot of a character with no entry alone.
//
// No checksum covers either table. The boot (0x42e8) checks the header at 0x3308
// ("VIRTUA FIGHTER 2", version 18 at 0x3306) and two CRC-16s (table-driven, 0x6d78): 0x3300
// over 0x3320..0x332e and 0x3302 over the settings at 0x3340..0x335c. A bad one sends the
// game to its factory reset (0x51de4), which also wipes both tables back to the factory
// times; nothing here touches those bytes. The VICTORY STREAK page (u16 per character at
// 0x39ec) has no initials and is not a ranking kept here.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'fvipers';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 16;
export const ENTRIES = 15;
export const TABLES = ['Ranking', 'Best time for each character'];

export const RANKING_AT = 0x3100;
export const CHARACTER_BEST_AT = 0x3280;
const CHARACTER_SLOTS = 8, SLOT_SIZE = 8, UNSET = 0xFFFFFFFF;
const MAX_TIME = 0x60000;   // the game stops adding at 360000; one more round goes past it

const INITIAL = /^[\x20-\x5f]{3}$/;
const CHARACTERS = ['Jane', 'Grace', 'Honey', 'Tokio', 'Bahn', 'Picky', 'Raxel', 'Sanman',
  'Mahler', 'B.M.'];

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

/* A character's slot as the 16-byte entry the service keeps, or null when unset. */
function slotEntry(backup, c) {
  const at = CHARACTER_BEST_AT + c * SLOT_SIZE;
  if (u32(backup, at) === UNSET) return null;
  const b = new Uint8Array(ENTRY_SIZE);
  b.set(backup.subarray(at, at + SLOT_SIZE), 0);
  b[10] = c;
  return parseEntry(b);
}

export function readTables(backup) {
  const ranking = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = RANKING_AT + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
    if (e) ranking.push(e);
  }
  const best = [];
  for (let c = 0; c < CHARACTER_SLOTS; c++) {
    const e = slotEntry(backup, c);
    if (e) best.push(e);
  }
  best.sort((a, b) => a.time - b.time);
  return [ranking, best];
}

/* Writes the Ranking (best first, 15 at most) and, for each character 0-7 with an entry
 * in the second table, its best one into its slot. No checksum to sign. */
export function writeTables(backup, tables) {
  const [ranking = [], best = []] = tables;
  for (let i = 0; i < ENTRIES && i < ranking.length; i++) {
    backup.set(hexToBytes(ranking[i].hex), RANKING_AT + i * ENTRY_SIZE);
  }
  const top = new Map();
  for (const e of best) {
    const b = hexToBytes(e.hex);
    if (!b || b.length !== ENTRY_SIZE || b[10] >= CHARACTER_SLOTS) continue;
    const prev = top.get(b[10]);
    if (!prev || e.time < prev.time) top.set(b[10], { time: e.time, bytes: b });
  }
  for (const [c, { bytes }] of top) {
    backup.set(bytes.subarray(0, SLOT_SIZE), CHARACTER_BEST_AT + c * SLOT_SIZE);
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
