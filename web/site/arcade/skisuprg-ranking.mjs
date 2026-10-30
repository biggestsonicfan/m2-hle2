// Sega Ski Super G's best times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like ranking.mjs (Sonic the Fighters'). m2-hle2's
// web build keeps the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.skisuprg).
// Read out of the skisuprg program (epr-19489/19490, 19551/19552 at 0x100000) for
// Pinboard #1911:
//
// Settings: two copies, each led by a u16 LE CRC, at 0x08 over [0x0A, 0x84) and at 0x84
// over [0x86, 0x100) (0x1b40 copies the first over the second). The CRC (0x10a00) is a
// shift register: g = 0xDEBDEB00; per byte g = (g & ~0xFF) + byte, then 8 steps; after
// the last byte 8 more steps; the result is g >> 16. A step is g*2, plus 0x10210000 when
// the top bit was set. At boot (0x1960) a good first copy is copied over the second when
// they differ; else a good second copy is copied back; two bad copies wipe all 16 KB
// (0x1a10) and put the defaults back.
//
// Ranking: 0x1E0 bytes at 0x110, four tables of 15 entries of 8 bytes (0x110, 0x188,
// 0x200, 0x278). Its checksum is a sum: (0x1234 + the 0x1E0 bytes) & 0xFFFF, a u16 LE at
// 0x82, inside the first settings copy (so it is mirrored at 0xFE and covered by both
// CRCs). The check at boot (0x5f200) puts the factory tables back when the sum is wrong,
// then has the settings signed again (0x510da0 = 1 -> 0x19f0 / 0x1a80). The game keeps its
// tables in work RAM (0x5730c0) and copies them out with the sum (0x5f030).
// An entry (insertion at 0x5eeb0; lower wins, 0 is an empty slot):
//   +0  u32 LE  the time as decimal digits MMSSCC (min*10000 + sec*100 + hundredths;
//               the display, 0x5ed50 / 0x5edc0, divides it that way)
//   +4  3 bytes initials as font codes (the font's table is in the data ROM at 0x2eda4b8,
//               39 entries): 0-9 the digits, 0x0A-0x23 A-Z, 0x24 '!', 0x25 '.', 0x26 '?';
//               0x27 is the space (the code at 0x2e7386c)
//   +7  u8      table 0: the sum of three per-round values (0x51a180..88, clamped 0..300)
//               divided by 10; tables 1-3: a byte from 0x52fe90. Neither is shown here.
// Which table (0x29340): when 0x521720 is 1 (the TIME ATTACK mode, by its bookkeeping
// line beside ROUND 1-3 and COMPLETE at 0x5a540) the table is the course number
// (0x530090, 1-3); otherwise a run through all three rounds goes into table 0. The
// courses are named in the order of the test menu's STAGE TIME lines (0x5a1ae).
// Factory: table 0 from 3'10"85; tables 1-3 from 0'40"88, 0'42"00 and 0'41"44 (made at
// 0x5f2xx from ROM 0xa46e0 and the names at 0xb47e0).
//
// The service's `time` is the time in hundredths; the entry's own bytes are written back.
// Played in MAME: first boot and attract only (the factory board, seed/skisuprg-backup1);
// the attract shows DRIVE BOARD TROUBLE (no drive board in MAME), so no ranking screen
// was seen and the table names follow the program, not the screen.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'skisuprg';
export const NVRAM_KEY = 'm2hle2.nvram.skisuprg';
export const ENTRY_SIZE = 8;
export const ENTRIES = 15;
export const RANKING_AT = 0x110;
export const RANKING_SIZE = 0x1E0;
export const COPIES = [[0x08, 0x0A, 0x84], [0x84, 0x86, 0x100]];   // CRC at, over [from, to)
export const SUM_AT = [0x82, 0xFE];
export const TABLES = ['Total (3 rounds)', 'Time attack: White Forest', 'Time attack: Night Valley',
  'Time attack: Wild King'];

export const CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!.? ';   // code n is CHARS[n]

export function crc16(bytes) {
  let g = 0xDEBDEB00;
  const step = () => { g = g & 0x80000000 ? (g * 2 + 0x10210000) >>> 0 : (g * 2) >>> 0; };
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) + x) >>> 0;
    for (let i = 0; i < 8; i++) step();
  }
  for (let i = 0; i < 8; i++) step();
  return g >>> 16;
}

export function rankingSum(backup) {
  let s = 0x1234;
  for (const x of backup.subarray(RANKING_AT, RANKING_AT + RANKING_SIZE)) s += x;
  return s & 0xFFFF;
}

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const u16 = (b, at) => b[at] | b[at + 1] << 8;
const put16 = (b, at, v) => { b[at] = v & 0xFF; b[at + 1] = v >> 8; };

/* One 8-byte entry -> { time (hundredths), initials, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const v = u32(bytes, 0), m = Math.floor(v / 10000), s = Math.floor(v / 100) % 100, cs = v % 100;
  if (v === 0 || s >= 60 || m > 99) return null;
  let initials = '';
  for (let i = 4; i < 7; i++) {
    if (bytes[i] >= CHARS.length) return null;
    initials += CHARS[bytes[i]];
  }
  return { time: (m * 60 + s) * 100 + cs, initials, hex: bytesToHex(bytes) };
}

export function copyValid(backup, copy) {
  const [at, from, to] = COPIES[copy];
  return u16(backup, at) === crc16(backup.subarray(from, to));
}

/* The four tables, as the game would keep them: none when both settings copies are bad
   (the board is wiped) or the ranking's sum is wrong (the factory tables come back). */
export function readTables(backup) {
  const copy = [0, 1].find((c) => copyValid(backup, c));
  const ok = copy !== undefined && u16(backup, SUM_AT[copy]) === rankingSum(backup);
  return TABLES.map((_, t) => {
    const out = [];
    for (let i = 0; ok && i < ENTRIES; i++) {
      const at = RANKING_AT + (t * ENTRIES + i) * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 15 each), the ranking's sum in both settings copies,
   and signs both copies again. */
export function writeTables(backup, tables) {
  tables.forEach((table, t) => {
    for (let i = 0; i < ENTRIES && i < (table || []).length; i++) {
      backup.set(hexToBytes(table[i].hex), RANKING_AT + (t * ENTRIES + i) * ENTRY_SIZE);
    }
  });
  const sum = rankingSum(backup);
  for (const at of SUM_AT) put16(backup, at, sum);
  for (const [at, from, to] of COPIES) put16(backup, at, crc16(backup.subarray(from, to)));
  return backup;
}

/* m'ss"cc from hundredths, as the game shows its MMSSCC. */
export function formatTime(cs) {
  const s = Math.floor(cs / 100) % 60;
  return `${Math.floor(cs / 6000)}'${String(s).padStart(2, '0')}"${String(cs % 100).padStart(2, '0')}`;
}
