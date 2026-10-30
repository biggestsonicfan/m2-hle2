// Manx TT Superbike's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.manxtt). Read out of the
// manxtt (Revision D) program, epr-18822d/18823d and epr-18824d/18825d (Pinboard #1911).
// Code addresses are the program ROM's.
//
// The board keeps two copies of 0x16AC bytes, at 0 and at 0x16AC (the rest is 0xFF). Each
// copy at B:
//   B+0      u16 CRC over B+2..B+0x148 (the settings); B+2 u16 the size, 0x16AC
//   B+0x148  a flag the game sets while it changes them
//   B+0x14C  u16 CRC over B+0x150..B+0x2D0 (bookkeeping)
//   B+0x2D0  a flag, as above, for the rankings
//   B+0x2D4  u16 CRC over B+0x2D8..B+0x16A8 (the four course blocks below); B+0x2D6 0xFFFF
// The CRC (0x6b9b0, table at ROM 0x6b520) is CCITT's 0x1021, from 0xFFFF, MSB first, the
// sum inverted. The boot (0x3230, check 0x3120: a wrong size or any bad CRC fails a copy)
// leaves a good first copy alone; a bad first copy with a good second gets the second
// (0x7b770 memcpy); both bad, all of it is reset to the factory (0x3010, bookkeeping too),
// signed (0x31c0) and copied over the second. The game saves (0x33a0) by signing the first
// copy and copying it over the second; writeTables writes the tables into both copies and
// signs each copy's ranking CRC, which leaves the settings and bookkeeping as they were.
//
// Four course blocks of 0x4F4 bytes at B+0x2D8 + 0x4F4 k, named in the bookkeeping
// screen's strings (ROM 0x73fa0..0x741a0): k 0 Laxey Coast, 1 TT Course (the race), 2 Laxey
// Coast, 3 TT Course ("T.T.Race Mode (Time Attack)"). Each block has a TOTAL TIME ranking
// at +0x134 and a LAP TIME one at +0x1AC, 10 entries of 12 bytes, best first (lower wins):
//   +0   u32 LE  the time in units of 1/56.4 s (the board's frame rate); the bookkeeping
//                screen (0x741c0) shows s = t / 56.4 as %2d'%02d'%02d: trunc(s / 60) % 60,
//                trunc(s) % 60, trunc(s * 100) % 100
//   +4   3 chars initials, then a NUL (factory ones include R&D, AM3, T.T)
//   +8   u16 LE  the gearbox: nonzero MT, 0 AT
//   +10  u16     0xFFFF, left as it is
//
// The factory fill (0x35b0) puts in each block, from ROM 0x33d0 + 0x78 k (10 x {u32 lap
// time, initials, u16 MT}), lap times as they are and total times = lap time x the laps
// the test menu sets for that course (+1 from the byte at RAM 0x514066 + k). The seed is a
// board at the default laps (2): its total times are twice the lap times. A board reset
// with other laps has other factory totals.
//
// Not played to a ranking in MAME (a bike race would need a riding script); the entries
// here are the factory ones. MAME's manxtt needs the default EEPROM image manxtt_dx_nvran,
// which this container's ROMs lack; the seed was made with a dummy (0xFF) one, which
// the backup RAM does not depend on. manxttc and manxttdx are not in this container's
// ROMs, so their layouts are not checked.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'manxtt';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 12;
export const ENTRIES = 10;

export const COPY_SIZE = 0x16AC;
export const COPIES = [0, COPY_SIZE];
export const RANKING_CRC_AT = 0x2D4;
export const COURSES_AT = 0x2D8;
export const COURSES_END = 0x16A8;
export const COURSE_SIZE = 0x4F4;
const TOTAL_AT = 0x134, LAP_AT = 0x1AC;

const COURSES = ['Laxey Coast', 'TT Course', 'Laxey Coast, time attack', 'TT Course, time attack'];
export const TABLES = COURSES.flatMap((c) => [`${c}: total time`, `${c}: lap time`]);
export const TABLE_ENTRIES = TABLES.map(() => ENTRIES);
export const TABLE_SCORE = TABLES.map(() => false);

const RATE = 56.4;
const MAX_TIME = 3600 * RATE;       // the screen shows minutes % 60
const INITIAL = /^[\x20-\x7e]{3}$/;

const u16 = (b, at) => b[at] | b[at + 1] << 8;
const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n << 8;
  for (let k = 0; k < 8; k++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
  return c & 0xFFFF;
});

/* The CRC of 0x6b9b0. */
export function crc16(bytes) {
  let c = 0xFFFF;
  for (const b of bytes) c = (CRC_TABLE[((c >> 8) ^ b) & 0xFF] ^ (c << 8)) & 0xFFFF;
  return ~c & 0xFFFF;
}

const SECTIONS = [[0, 2, 0x148], [0x14C, 0x150, 0x2D0], [RANKING_CRC_AT, COURSES_AT, COURSES_END]];

/* True when the copy at `base` passes the boot's check (0x3120). */
export function copyValid(backup, base = 0) {
  if (u16(backup, base + 2) !== COPY_SIZE) return false;
  return SECTIONS.every(([at, from, to]) =>
    u16(backup, base + at) === crc16(backup.subarray(base + from, base + to)));
}

const tableAt = (t) => COURSES_AT + (t >> 1) * COURSE_SIZE + (t & 1 ? LAP_AT : TOTAL_AT);

/* One 12-byte entry -> { time, initials, mt, hex }, or null. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[7] !== 0) return null;
  const time = u32(bytes, 0), initials = String.fromCharCode(bytes[4], bytes[5], bytes[6]);
  if (time === 0 || time >= MAX_TIME || !INITIAL.test(initials)) return null;
  return { time, initials, mt: u16(bytes, 8) !== 0, hex: bytesToHex(bytes) };
}

/* The tables, from the copy the game would keep (the first, else the second; neither
 * good, the boot resets the board, so it has none). */
export function readTables(backup) {
  const base = COPIES.find((c) => copyValid(backup, c));
  return TABLES.map((_, t) => {
    const out = [];
    if (base === undefined) return out;
    for (let i = 0; i < ENTRIES; i++) {
      const at = base + tableAt(t) + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes each table (best first, 10 at most) into both copies and signs their rankings. */
export function writeTables(backup, tables) {
  if (!copyValid(backup, 0) && copyValid(backup, COPY_SIZE)) {
    backup.copyWithin(0, COPY_SIZE, 2 * COPY_SIZE);   // as the boot would
  }
  for (const base of COPIES) {
    TABLES.forEach((_, t) => {
      const fit = (tables[t] || []).map((e) => parseEntry(hexToBytes(e.hex))).filter(Boolean);
      for (let i = 0; i < ENTRIES && i < fit.length; i++) {
        backup.set(hexToBytes(fit[i].hex), base + tableAt(t) + i * ENTRY_SIZE);
      }
    });
    const c = crc16(backup.subarray(base + COURSES_AT, base + COURSES_END));
    backup[base + RANKING_CRC_AT] = c & 0xFF;
    backup[base + RANKING_CRC_AT + 1] = c >> 8;
  }
  return backup;
}

/* A time in 1/56.4 s as the game shows it, here m'ss"cc like the other games. */
export function formatTime(t) {
  const s = t / RATE;
  const m = Math.trunc(s / 60) % 60, ss = Math.trunc(s) % 60, cc = Math.trunc(s * 100) % 100;
  return `${m}'${String(ss).padStart(2, '0')}"${String(cc).padStart(2, '0')}`;
}

export function detail(e) { return e ? (e.mt ? 'MT' : 'AT') : ''; }
