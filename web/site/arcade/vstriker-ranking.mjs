// Virtua Striker's ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.vstriker). Read out of the
// program the container has (MAME's vstriker, Revision A) (Pinboard #1911):
//
// The game keeps two copies of a 0x80-byte block, at 0x200 and 0x280:
//   +0x00 FF FF FF FF
//   +0x04 u16 LE  CRC over +0x06 to the end of the block (0x7A bytes): the routine at
//         0x73860, table in the data ROM at 0x283c5a0, is CRC-16/XMODEM (poly 0x1021,
//         MSB first, seeded 0, not inverted)
//   +0x0A 10 names of 3 character codes, best first
//   +0x28 10 bytes: the round the player reached: 3 FINAL, 2 SEMI FINAL, 1 2nd MATCH,
//         0 1st MATCH
//   +0x32 10 bytes: goals, shown as "+8"
//   +0x3C 10 bytes: the player's team, shown as its flag (0 DEN ... 0x11 ARG, 0x12 SEG;
//         the abbreviations at 0x283c030 are what the name entry falls back to)
//   +0x46 FF to the end of the block
// At boot (0x72c40) a good first copy is kept; a bad one is replaced by a good second; two
// bad copies get the factory ranking (0x72a10, data ROM 0x283c460, 7 bytes an entry:
// BRA FINAL +8 to COL 1st MATCH +2). After a name entry the game writes the table
// (0x72b80), signs the first copy and copies it over the second (0x72dd0).
// The ranking is sorted (0x71a58) by goals, then by round, both higher first; a new entry
// goes above the ones it ties with.
//
// Character codes (the name entry's grid is at 0x283c360): 0x00-0x09 the digits,
// 0x0A-0x23 A to Z, 0x24 '.', 0x25 '!', 0x26 a space (0x27 is the grid's END). A board
// written with ". !", "059" and " A " was booted in MAME and showed them so, and kept them;
// one written by writeTables (ROB FINAL +9 JPN) was kept too. Not played in MAME for an
// entry the game wrote itself.
//
// The service keeps an entry as 6 bytes: the 3 name codes, round, goals, team. It sorts by
// `time` ascending, so `time` is KEY - (goals * 4 + round) (SCORE); formatTime shows the
// goals and the round, detail the team.
//
// Only Revision A keeps a ranking in backup RAM: the original release (MAME's vstrikro,
// m2-hle2's vstrikero profile) writes nothing at 0x200 (no reference to 0x1d00200-0x1d002ff
// in its program; its first board is FF there).

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'vstriker';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 6;
export const ENTRIES = 10;
export const COPY_SIZE = 0x80;
export const COPIES = [0x200, 0x280];
export const TABLES = ['Ranking'];
const NAMES_AT = 0x0A, ROUND_AT = 0x28, GOAL_AT = 0x32, TEAM_AT = 0x3C;

export const SCORE = true;      // higher wins: `time` is KEY - (goals * 4 + round) (games.mjs)
const KEY = 0xFFFFFFFF;
const MAX_GOALS = 99;           // a byte; the ranking shows two digits
const ROUNDS = ['1st MATCH', '2nd MATCH', 'SEMI FINAL', 'FINAL'];
const TEAMS = ['DEN', 'ENG', 'FRA', 'SPA', 'ITA', 'NIG', 'SWE', 'GER', 'NET', 'BUL', 'SAU',
  'KOR', 'JPN', 'USA', 'MEX', 'COL', 'BRA', 'ARG', 'SEG'];
const CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.! ';

/* The program's CRC: CRC-16/XMODEM. */
export function crc16(bytes) {
  let c = 0;
  for (const x of bytes) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c;
}

/* True when a copy carries its own CRC, as the game checks it. */
export function copyValid(backup, copy) {
  return (backup[copy + 4] | backup[copy + 5] << 8) === crc16(backup.subarray(copy + 6, copy + COPY_SIZE));
}

/* One 6-byte entry (3 name codes, round, goals, team) -> { time, initials, round, goals,
 * team, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const [a, b, c, round, goals, team] = bytes;
  if (a >= CHARS.length || b >= CHARS.length || c >= CHARS.length) return null;
  if (round > 3 || goals > MAX_GOALS || team >= TEAMS.length) return null;
  return {
    time: KEY - (goals * 4 + round), initials: CHARS[a] + CHARS[b] + CHARS[c],
    round, goals, team, hex: bytesToHex(bytes),
  };
}

/* The table, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  const out = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = copy + NAMES_AT + 3 * i;
    const e = parseEntry(Uint8Array.of(backup[at], backup[at + 1], backup[at + 2],
      backup[copy + ROUND_AT + i], backup[copy + GOAL_AT + i], backup[copy + TEAM_AT + i]));
    if (e) out.push(e);
  }
  return [out];
}

/* Writes the table (best first, 10) into both copies and signs each again. */
export function writeTables(backup, tables) {
  const table = (tables[0] || []).map((e) => parseEntry(hexToBytes(e.hex))).filter(Boolean);
  for (const copy of COPIES) {
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      const b = hexToBytes(table[i].hex);
      backup.set(b.subarray(0, 3), copy + NAMES_AT + 3 * i);
      backup[copy + ROUND_AT + i] = b[3];
      backup[copy + GOAL_AT + i] = b[4];
      backup[copy + TEAM_AT + i] = b[5];
    }
    const c = crc16(backup.subarray(copy + 6, copy + COPY_SIZE));
    backup.set([c & 0xFF, c >> 8], copy + 4);
  }
  return backup;
}

/* The goals and the round, from the key the service sorts by ("+8 FINAL"). */
export function formatTime(time) {
  const v = KEY - time;
  return `+${v >> 2} ${ROUNDS[v & 3]}`;
}

export function teamName(t) { return TEAMS[t] || ''; }

export function detail(entry) { return entry && entry.team !== undefined ? teamName(entry.team) : ''; }
