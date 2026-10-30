// Top Skater's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.topskatr). Read out of the
// topskatr (Export, Revision A) program (Pinboard #1911); topskatru (USA, Revision A) has
// the same layout and the same factory board in MAME, byte for byte.
//
// The game keeps two copies of a 0xFA8-byte block, A at 0 and B at 0xFA8. A copy is
// three checksummed parts, each checksum a u16 LE CRC-16/GENIBUS (poly 0x1021, init
// 0xFFFF, the result inverted; table at ROM 0x96e70, routine 0x97310):
//   +0      CRC of +2 .. +0x188 (+2 is the u16 0x0FA8, the block's size)
//   +0x188  u32 "changed" flag of the second part
//   +0x18C  CRC of +0x190 .. +0x238 (settings)
//   +0x238  u32 "changed" flag of the third part (the factory fill and a new entry set it)
//   +0x23C  CRC of +0x240 .. +0xFA8 (the rankings, then the bookkeeping from +0xE1C)
// 0x13c90 checks the size and the three CRCs. At boot (0x13e70) a bad A is replaced by a
// good B; two bad copies get the factory board (0x13bb0; the rankings from 0x9d720),
// signed in A (0x13d20) and copied to B. The save (0x13f70) signs A, recomputing a part's
// CRC only when its flag is set (then cleared), and copies the whole of A over B.
// The boot also invalidates both copies (u16 0 at 0 and 0xFA8) when the serial EEPROM
// lacks "SEGA"/"TPSK" or fails its own CRC (0x5b330): a board kept without its EEPROM
// comes back factory.
//
// Three rankings, one per course, 100 entries of 8 bytes, best first, at
// 0x4B4 + 8 * (rank + 1) + 800 * course (the course at RAM 0x5830a4; the debug strings
// "HISCORE SCORE EASY/HARD/HIDE" give the order). The attract mode and 0xabcbc show the
// top 20 only.
//   +0  u32 LE  score, HIGHER wins, shown in 7 digits
//   +4  u8      the skater (0x509d60[n] names it): 0 Ash, 1 Keith, 2 Kenta, 3 J, 4 Cookie,
//               5 Jill, 6 Alex, 7 Pnut (the order of the bookkeeping's "CHARA NINKI"
//               counters and of the announcer's DJ08_ASH .. DJ15_PNUT samples)
//   +5  3 bytes initials, a code each (0x9d688): 0-25 A-Z, 26-35 '0'-'9', 36 '!', 37 '?',
//               38 '.', 39 space
// The factory board (0x9d720): entries 0-19 from ROM tables of {u32 score, skater, 3
// ASCII} (EASY 0x9d540: BOY 250000 .. TKO 202500; HARD 0x9d4a0: OGU 250000 .. AM3
// 202500; HIDE 0x9d5e0: OGU 200000 .. AM3 162000); entries 20-99 three spaces, a random
// skater 0-5, entry 19's score less 1250 a rank (750 on HIDE).
//
// The service sorts by `time` ascending, so an entry's `time` is KEY - score.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'topskatr';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 8;
export const ENTRIES = 100;
export const COPY_SIZE = 0xFA8;
export const COPIES = [0, COPY_SIZE];
export const RANKING_AT = 0x4BC;             // in a copy
export const TABLE_SIZE = ENTRIES * ENTRY_SIZE;
export const TABLES = ['Easy', 'Hard', 'Hide'];
export const SCORE = true;     // higher wins: `time` is KEY - score (games.mjs)
export const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!?. ';   // code n is CHARS[n]
const SKATERS = ['Ash', 'Keith', 'Kenta', 'J', 'Cookie', 'Jill', 'Alex', 'Pnut'];

const KEY = 0xFFFFFFFF;
const MAX_SCORE = 9999999;     // seven digits on the ranking
const PARTS = [[0, 2, 0x188], [0x18C, 0x190, 0x238], [0x23C, 0x240, COPY_SIZE]];   // crc, from, to
const RANKING_PART = 2, RANKING_FLAG = 0x238;

/* CRC-16/GENIBUS, as the program's table-driven routine (0x97310). */
export function crc16(bytes) {
  let c = 0xFFFF;
  for (const x of bytes) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c ^ 0xFFFF;
}

const u16 = (b, at) => b[at] | b[at + 1] << 8;
const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* What 0x13c90 checks: the size and the three CRCs. */
export function copyValid(backup, copy) {
  if (u16(backup, copy + 2) !== COPY_SIZE) return false;
  return PARTS.every(([at, from, to]) =>
    u16(backup, copy + at) === crc16(backup.subarray(copy + from, copy + to)));
}

/* One 8-byte entry -> { time, score, initials, skater, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const score = u32(bytes, 0), skater = bytes[4];
  if (score === 0 || score > MAX_SCORE || skater >= SKATERS.length) return null;
  let initials = '';
  for (let i = 5; i < 8; i++) {
    if (bytes[i] >= CHARS.length) return null;
    initials += CHARS[bytes[i]];
  }
  return { time: KEY - score, score, initials, skater, hex: bytesToHex(bytes) };
}

/* The three rankings, from the first good copy (from A if neither is good). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  return TABLES.map((_, t) => {
    const out = [];
    for (let i = 0; i < ENTRIES; i++) {
      const at = copy + RANKING_AT + t * TABLE_SIZE + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes each ranking (best first, 100 at most) into the good copy, signs the rankings'
 * part (its flag cleared, as the save leaves it) and copies the copy over the other. */
export function writeTables(backup, tables) {
  const [a, b] = COPIES;
  const from = !copyValid(backup, a) && copyValid(backup, b) ? b : a, to = from === a ? b : a;
  TABLES.forEach((_, t) => {
    const table = tables[t] || [];
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      backup.set(hexToBytes(table[i].hex), from + RANKING_AT + t * TABLE_SIZE + i * ENTRY_SIZE);
    }
  });
  backup.fill(0, from + RANKING_FLAG, from + RANKING_FLAG + 4);
  const [at, start, end] = PARTS[RANKING_PART];
  const c = crc16(backup.subarray(from + start, from + end));
  backup.set([c & 0xFF, c >> 8], from + at);
  backup.copyWithin(to, from, from + COPY_SIZE);
  return backup;
}

/* The score, from the key the service sorts by. */
export function formatTime(time) { return String(KEY - time); }

export function skaterName(n) { return SKATERS[n] || ''; }

export function detail(entry) { return entry ? skaterName(entry.skater) : ''; }
