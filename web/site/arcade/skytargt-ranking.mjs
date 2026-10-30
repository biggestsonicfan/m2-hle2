// Sky Target's ranking (ACE PILOTS), as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like vcop-ranking.mjs. m2-hle2's web build keeps the
// 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.skytargt). Read out of the
// skytargt program, epr-18406/18407 (Pinboard #1911). Code addresses are the program
// ROM's.
//
// The ranking block: a u32 at 0x10C, the checksum (u16, the high half zero: the boot
// compares all 32 bits), over the 0xA0 bytes at 0x110..0x1B0; a copy of the 0xA4 bytes
// (checksum too) at 0x1B0. The checksum (0x13890) starts from 0xDEBDEB00 and, for each
// byte, puts the byte in the low 8 bits and shifts eight times, adding 0x10210000 when the
// top bit falls out; then eight more shifts; the sum is the top 16 bits. The boot
// (0x74b90): a good block whose copy's checksum differs is re-signed and copied over the
// copy (0x74c90, 0x74d80); a bad block with a good copy gets the copy (0x74dd0); both
// bad, 0x74cc0 clears 0x10C..0x254, puts the factory table in (0xA0 bytes from ROM
// 0xebe10) and signs it. The game works on a copy in RAM (0x527960) and writes it back
// with 0x74b00 (copy, then sign both). writeTables does the same. The settings at 0x8
// (u16 checksum over 0xA..0x54, copy at 0x54) are left alone.
//
// 10 entries of 16 bytes at 0x110, best first (0x57480 puts a new one in: higher score
// wins, on equal scores more kills); the attract mode's ACE PILOTS screen shows RANK,
// NAME, SCORE, FIGHTER and KILL, five a page:
//   +0   3 chars initials, then a NUL (the screen draws char - 0x20 glyphs, 0x21bc0;
//        factory ones Y.H HAT AO! A.S BOK TAG TOZ TAK NAV NAI)
//   +4   u32 LE  the score / 10: the screen shows it with a 0 after (the factory 10000
//                is shown 100000). `score` here is the score as shown
//   +8   u32 LE  the fighter: 0 F-14D, 1 F-15S/MTD, 2 F-16C, 3 RAFALE M (ROM 0xd5140)
//   +12  u32 LE  kills, shown as three digits (factory 100 down to 10)
//
// `time` (the service's key, lowest first) = (9999999 - score / 10) * 1000 + 999 - kills,
// so a higher score, then more kills, comes first.
//
// Not played to a ranking in MAME (a rail shooter run would need an aiming script); the
// entries here are the factory ones. Sky Target has no clone in MAME.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'skytargt';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 16;
export const ENTRIES = 10;
export const SCORE = true;
export const TABLES = ['Ace pilots'];

export const BLOCK_AT = 0x10C;          // u32 checksum, then the table it covers
export const RANKING_AT = 0x110;
export const BLOCK_SIZE = 0xA4;
export const COPY_AT = 0x1B0;

export const FIGHTERS = ['F-14D', 'F-15S/MTD', 'F-16C', 'RAFALE M'];
const MAX_SCORE = 9999999;          // as kept, shown with a 0 after
const MAX_KILLS = 999;
const INITIAL = /^[\x20-\x7e]{3}$/;

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* The checksum of 0x13890. */
export function checksum(bytes) {
  let g = 0xDEBDEB00;
  const step = () => { g = g > 0x7FFFFFFF ? (g * 2 + 0x10210000) >>> 0 : (g * 2) >>> 0; };
  for (const b of bytes) {
    g = ((g & 0xFFFFFF00) + b) >>> 0;
    for (let k = 0; k < 8; k++) step();
  }
  for (let k = 0; k < 8; k++) step();
  return g >>> 16;
}

/* True when the block at `at` (BLOCK_AT or COPY_AT) matches its checksum. */
export function blockValid(backup, at = BLOCK_AT) {
  return u32(backup, at) === checksum(backup.subarray(at + 4, at + BLOCK_SIZE));
}

/* One 16-byte entry -> { time, score, initials, fighter, kills, hex }, or null. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[3] !== 0) return null;
  const initials = String.fromCharCode(bytes[0], bytes[1], bytes[2]);
  const kept = u32(bytes, 4), fighter = u32(bytes, 8), kills = u32(bytes, 12);
  if (!INITIAL.test(initials) || kept === 0 || kept > MAX_SCORE) return null;
  if (fighter >= FIGHTERS.length || kills > MAX_KILLS) return null;
  return { time: (MAX_SCORE - kept) * 1000 + MAX_KILLS - kills, score: kept * 10, initials,
    fighter, kills, hex: bytesToHex(bytes) };
}

/* The ranking, from the block the game would keep (the first, else its copy; neither, the
 * boot puts the factory table back, so it has none). */
export function readTables(backup) {
  const at = blockValid(backup, BLOCK_AT) ? BLOCK_AT : blockValid(backup, COPY_AT) ? COPY_AT : -1;
  const out = [];
  for (let i = 0; at >= 0 && i < ENTRIES; i++) {
    const p = at + 4 + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(p, p + ENTRY_SIZE));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the ranking (best first, 10 at most), signs it and copies it to 0x1B0. */
export function writeTables(backup, tables) {
  if (!blockValid(backup, BLOCK_AT) && blockValid(backup, COPY_AT)) {
    backup.copyWithin(BLOCK_AT, COPY_AT, COPY_AT + BLOCK_SIZE);   // as the boot would
  }
  const fit = (tables[0] || []).map((e) => parseEntry(hexToBytes(e.hex))).filter(Boolean);
  for (let i = 0; i < ENTRIES && i < fit.length; i++) {
    backup.set(hexToBytes(fit[i].hex), RANKING_AT + i * ENTRY_SIZE);
  }
  const c = checksum(backup.subarray(RANKING_AT, BLOCK_AT + BLOCK_SIZE));
  backup.set([c & 0xFF, c >> 8, 0, 0], BLOCK_AT);
  backup.copyWithin(COPY_AT, BLOCK_AT, BLOCK_AT + BLOCK_SIZE);
  return backup;
}

/* The score as the screen shows it, from parseEntry's key. */
export function formatTime(t) { return String((MAX_SCORE - Math.floor(t / 1000)) * 10); }

export function fighterName(f) { return FIGHTERS[f] || ''; }

export function detail(e) { return e ? `${fighterName(e.fighter)}, ${e.kills} kills` : ''; }
