// Sega Water Ski's high scores, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like ranking.mjs (Sonic the Fighters'). m2-hle2's
// web build keeps the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.segawski).
// Read out of the segawski program (Revision A, epr-19963a/19964a) for Pinboard #1911:
//
// Settings: a u16 LE CRC at 0x08 over [0x0A, 0x40), and a copy of 0x08..0x40 at 0x40.
// The CRC (0x2dac0) is a shift register: g = 0xDEBDEC00; per byte g = (g & ~0xFF) + byte,
// then 8 steps (g*2, plus 0x10210000 when the top bit was set); the result is g >> 16.
// At boot (0xdeb0) a good first copy is signed and copied over the second when their
// CRCs differ; else a good second copy is copied back; two bad copies wipe 0..0x1C9F
// (0xdfb0), refill the factory ranking (0x5fbb0) and sign the settings. 0x04 is a u32
// that changes as the game runs, outside the CRC.
//
// Ranking: three tables of 100 entries of 12 bytes at 0x7C, 0x52C and 0x9DC (0x4B0 each).
// The game reads and writes them in place (0x572988 points at 0x1D0007C). Nothing checks
// them: a CRC slot at 0x78 over [0x7A, 0xE8C) and a mirror at 0xE8C (0xdf90, 0xe030,
// 0xe0b0) exist, but no code calls them, and the factory board has zeros there.
// An entry (insertion at 0x5fcf0, called from 0x5c340; higher wins, a tie goes above):
//   +0  3 bytes initials as font codes: 0-25 A-Z, 0x24 '.', and (by the default name
//               0x00 0x0C 0x1B = "AM1" at 0x5c240) 26-35 the digits; 0x27 pads an
//               unfinished name, shown here as a space
//   +3  0
//   +4  u32 LE  points (factory 10000 down by 90)
//   +8  u8      level 0-5 as the game worked it out (0x56e168)
//   +9  u8      skier 0-3: TAKUTO, ROZZMAN, SHAMPOO, LENNY (0x56e090)
//   +10 u16     zero
// Which table: 0x56e094 & 3 (the course: 初級, beginner, is table 0, "PALM BEACH" on its
// ranking screen). The LEVEL column the attract shows (S A B C D E) follows the points
// and the table's thresholds at 0xf0160 (10000, 8000, 6000, 4000, 2000; 15000, 13000,
// 11000, 8000, 5000; 17000, 15000, 13000, 1000, 7000): the factory's 10000 shows as S
// though its stored level is 1, so detail() grades the points as the screen does.
//
// Played in MAME: first boot and the attract (the factory board, seed/segawski-backup1);
// the attract's ranking screen showed table 0 as read here. The sound program
// epr-19967.31 in the container's zip has a wrong checksum; the game runs regardless.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'segawski';
export const NVRAM_KEY = 'm2hle2.nvram.segawski';
export const ENTRY_SIZE = 12;
export const ENTRIES = 100;
export const TABLES_AT = [0x07C, 0x52C, 0x9DC];
export const COPIES = [[0x08, 0x0A, 0x40], [0x40, 0x42, 0x78]];   // CRC at, over [from, to)
// The second and third names are not seen in the program's text (the courses are art);
// they follow 初級 as the levels of difficulty usually do.
export const TABLES = ['Beginner (Palm Beach)', 'Intermediate', 'Advanced'];

export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_SCORE = 99999999;

export const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.?? ';   // code n is CHARS[n]
export const SKIERS = ['TAKUTO', 'ROZZMAN', 'SHAMPOO', 'LENNY'];
const LEVELS = 'SABCDE';
const THRESHOLDS = [[10000, 8000, 6000, 4000, 2000], [15000, 13000, 11000, 8000, 5000],
  [17000, 15000, 13000, 1000, 7000]];

export function crc16(bytes) {
  let g = 0xDEBDEC00;
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) + x) >>> 0;
    for (let i = 0; i < 8; i++) g = g & 0x80000000 ? (g * 2 + 0x10210000) >>> 0 : (g * 2) >>> 0;
  }
  return g >>> 16;
}

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const u16 = (b, at) => b[at] | b[at + 1] << 8;

/* One 12-byte entry -> { time, score, initials, skier, table, hex }, or null. */
export function parseEntry(bytes, table = 0) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  if (bytes[3] || bytes[10] || bytes[11] || bytes[8] > 5 || bytes[9] > 3) return null;
  const score = u32(bytes, 4);
  if (score === 0 || score > MAX_SCORE) return null;
  let initials = '';
  for (let i = 0; i < 3; i++) {
    if (bytes[i] >= CHARS.length) return null;
    initials += CHARS[bytes[i]];
  }
  return { time: KEY - score, score, initials, skier: bytes[9], table: table | 0, hex: bytesToHex(bytes) };
}

export function copyValid(backup, copy) {
  const [at, from, to] = COPIES[copy];
  return u16(backup, at) === crc16(backup.subarray(from, to));
}

/* The three tables; none when both settings copies are bad (the game would wipe it). */
export function readTables(backup) {
  const ok = copyValid(backup, 0) || copyValid(backup, 1);
  return TABLES_AT.map((at, t) => {
    const out = [];
    for (let i = 0; ok && i < ENTRIES; i++) {
      const e = parseEntry(backup.subarray(at + i * ENTRY_SIZE, at + (i + 1) * ENTRY_SIZE), t);
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 100 each). Nothing signs them; the settings and their
   CRCs are left as they are. */
export function writeTables(backup, tables) {
  TABLES_AT.forEach((at, t) => {
    const table = tables[t] || [];
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      backup.set(hexToBytes(table[i].hex), at + i * ENTRY_SIZE);
    }
  });
  return backup;
}

/* The points, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

/* The LEVEL letter as the ranking screen shows it, and the skier. */
export function detail(entry) {
  if (!entry || entry.score === undefined) return '';
  const th = THRESHOLDS[entry.table] || THRESHOLDS[0];
  const k = th.findIndex((x) => entry.score >= x);
  return `${LEVELS[k < 0 ? 5 : k]} ${SKIERS[entry.skier] || ''}`.trim();
}
