// Air Walkers' ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps the
// 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.airwlkrs). Read out of the
// airwlkrs program (Pinboard #1091):
//
// The board starts with "SCG1" and a u16 6. The game keeps a 0xEC-byte block twice:
//   A  at 0x12..0xFC, signed by a u16 LE checksum at 0x10;
//   A' at 0xFE..0x1E8, signed by a u16 LE checksum at 0xFC.
// The checksum (0xBFDB0) is a CRC-CCITT shifted bit by bit through a 32-bit register seeded
// 0xDEBDEB00: each byte goes into the low 8 bits, then 8 steps of "shift left, xor 0x10210000
// when bit 31 comes out set"; 8 more steps at the end, the result is the top 16 bits.
// At boot (0xBF8E0): a bad header means a factory fill; A good and A' bad copies A over A'
// (0xBFC30); A bad and A' good copies A' over A (0xBFC80); both bad is a factory fill
// (0xBFAF0: zeroes 0..0xC8C, the defaults, the ranking from ROM 0xFA0F0).
//
//   0xC0 (A) and 0x1AC (A'): RANKING, 5 entries of 12 bytes, best first:
//     +0  u16 LE score, 1..999 (0xE7390 sums the team's results and caps them at 999; a
//         new score goes in above the first entry it beats strictly, so a tie goes below)
//     +2  3 name chars of player 1, +5 3 name chars of player 2 (0xFF 0xFF 0xFF: no
//         player 2). The chars index the font table at ROM 0xFBFE0: 0-25 A-Z, then
//         / = * - + : ! . & _ space | {. Shown "P1&P2" ("H.K&XYZ") or "P1" ("S.T").
//     +8  u8 round reached, shown as a badge (1 CONF. SEMIFINAL, 2 CONF. FINAL,
//         4 CHAMPION, 5 LEGEND; 0 and 3 not seen, by the order FIRST ROUND and FINAL)
//     +9  u8 team, 0-15 (the table at ROM 0xFBA80)
//     +10 two zero bytes
// The factory ranking: S.T 100, H.K&XYZ 80, A.K&AAA 60, K.T 40, K.O&ZZZ 20.
// (0x1EC on: the bookkeeping, with its own checksum at 0x1E8; not touched here.)
//
// A HIGHER score wins here, and the service sorts by `time`, lower first: parseEntry's
// `time` is the key 0xFFFF - score (and `score` is the score); formatTime shows the score.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'airwlkrs';
export const NVRAM_KEY = 'm2hle2.nvram.airwlkrs';
export const TABLES = ['Ranking'];
export const ENTRY_SIZE = 12;
export const ENTRIES = 5;
export const SCORE = true;
export const COPIES = [0xC0, 0x1AC];
// [checksum at, signs from, to]
export const BLOCKS = [[0x10, 0x12, 0xFC], [0xFC, 0xFE, 0x1E8]];

const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ/=*-+:!.&_ |{';
const ROUNDS = ['First round', 'Conf. semifinal', 'Conf. final', 'Final', 'Champion', 'Legend'];
const TEAMS = ['Seattle Velocity', 'Utah Groovers', 'San Antonio Skeletons', 'Houston Thunderbolts',
  'Phoenix Guillotiners', 'Los Angeles Crawfish', 'Portland Tomahawks', 'Denver Hurricanes',
  'Chicago Red Flags', 'Orlando Megaremix', 'New York Stars', 'Indiana Vultures',
  'Atlanta Hammers', 'Cleveland Knights', 'Charlotte Meteors', 'Boston Jesters'];

/* The checksum of 0xBFDB0. */
export function checksum(bytes) {
  let c = 0xDEBDEB00;
  const step = () => { c = (c * 2) >>> 0; if (c & 0x80000000) c = (c ^ 0x10210000) >>> 0; };
  for (const b of bytes) {
    c = ((c & 0xFFFFFF00) | b) >>> 0;
    for (let k = 0; k < 8; k++) step();
  }
  for (let k = 0; k < 8; k++) step();
  return c >>> 16;
}

const u16 = (b, at) => b[at] | b[at + 1] << 8;

/* True when block i (0: A, 1: A') matches its checksum. */
export function blockValid(backup, i) {
  const [at, from, to] = BLOCKS[i];
  return u16(backup, at) === checksum(backup.subarray(from, to));
}

function name(b, at) {
  let s = '';
  for (let i = 0; i < 3; i++) {
    if (b[at + i] >= CHARS.length) return null;
    s += CHARS[b[at + i]];
  }
  return s;
}

/* One 12-byte entry -> { time, score, initials, round, team, hex } or null. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const score = u16(bytes, 0), round = bytes[8], team = bytes[9];
  if (score < 1 || score > 999 || round >= ROUNDS.length || team >= TEAMS.length) return null;
  if (bytes[10] || bytes[11]) return null;
  const p1 = name(bytes, 2);
  if (!p1 || !p1.trim()) return null;
  let initials = p1.trim();
  if (!(bytes[5] === 0xFF && bytes[6] === 0xFF && bytes[7] === 0xFF)) {
    const p2 = name(bytes, 5);
    if (!p2 || !p2.trim()) return null;
    initials += '&' + p2.trim();
  }
  return { time: 0xFFFF - score, score, initials, round, team, hex: bytesToHex(bytes) };
}

/* The ranking, from the block the game would keep (A, else A'). */
export function readTables(backup) {
  const copy = blockValid(backup, 0) || !blockValid(backup, 1) ? COPIES[0] : COPIES[1];
  const out = [];
  for (let i = 0; i < ENTRIES; i++) {
    const e = parseEntry(backup.subarray(copy + i * ENTRY_SIZE, copy + (i + 1) * ENTRY_SIZE));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the ranking (best first, 5 entries) into both blocks and signs both. */
export function writeTables(backup, tables) {
  const fit = (tables[0] || []).map((e) => hexToBytes(e.hex)).filter((b) => b.length === ENTRY_SIZE);
  for (const copy of COPIES) {
    for (let i = 0; i < ENTRIES && i < fit.length; i++) backup.set(fit[i], copy + i * ENTRY_SIZE);
  }
  for (const [at, from, to] of BLOCKS) {
    const c = checksum(backup.subarray(from, to));
    backup[at] = c & 0xFF;
    backup[at + 1] = c >> 8;
  }
  return backup;
}

/* The score, from parseEntry's key. */
export function formatTime(t) { return String(0xFFFF - t); }

export function teamName(t) { return TEAMS[t] || ''; }
export function roundName(r) { return ROUNDS[r] || ''; }

export function detail(e) { return e ? teamName(e.team) : ''; }
