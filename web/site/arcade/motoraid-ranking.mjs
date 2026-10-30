// Motor Raid's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.motoraid). Read out of the
// motoraid (Twin) program, epr-20007/20008 (Pinboard #1911). Code addresses are the
// program ROM's; 0x1d00xxx is the backup RAM.
//
// The ranking block is 0x484 bytes at 0x1C4: a u16 checksum at 0x1C4 over the 0x482
// bytes after it (0x1C6..0x648; 0x1C6 is two zero bytes, the tables start at 0x1C8), and
// a copy of the whole block (checksum too) at 0x648. The checksum (0x1f310) starts from
// 0xDEBDEC00 and, for each byte, puts the byte in the low 8 bits and shifts eight times,
// adding 0x10210000 when the top bit falls out; the sum is the top 16 bits. The boot
// (0xca60, check 0xcacc): a good block whose copy's checksum differs is re-signed and
// copied over the copy (0xcbb0, 0xcc90); a bad block with a good copy gets the copy back
// (0xcd10); both bad, the factory tables (0x49d20: 0x480 bytes from ROM 0x46a00 or
// 0x46580, the same bytes) go in and are signed (0xcb80). writeTables writes the block,
// signs it and copies it to 0x648 as the game does. The settings at 0x8 (u16 checksum over
// 0xA..0xD8, copy at 0xD8) are left alone.
//
// Six tables of 8 entries of 24 bytes, one every 0xC0 from 0x1C8, best first. The
// ranking screen (0x764b0) shows a course's title (sprite 0x76120[table]) and its rows
// (0x76560); the tables' order is the courses' (found by giving each table other initials
// and watching the attract ranking): JUNOS, YENDAS, IDO, REEF8, BOWEL, then the
// championship.
//
//  Course tables 0..4, lower time wins (0x492a0 compares a time with the last entry's,
//  +0xAC in the table):
//   +0   3 chars initials, then a NUL (factory ones: WAT MAT OOH KIT IJI T.O HAY YAS)
//   +4   f32 LE  the time in seconds; 0x76960 shows it m'ss"cc, all three truncated:
//                m = trunc(t / 60), ss = trunc(t - 60 m), cc = trunc((t - trunc(t)) * 100).
//                The factory times run 420..490 s (JUNOS, BOWEL), 300..370 (YENDAS,
//                REEF8), 360..430 (IDO), by 10
//   +8   u8      the rider (& 3 picks the portrait, table 0x76140; factory 0 2 3 1 ...)
//   +9.. zero in the factory tables, kept as they are
//
//  CHAMPIONSHIP, table 5 (0x588), higher points win (0x49480 compares new > last):
//   +0   3 chars initials, then a NUL (factory RBN GNZ GLA I.O twice)
//   +4.. zero
//   +0xC u32 LE  points (factory 3000 for all eight)
//   +0x10 u8     1 in the factory table
//   +0x11 the courses raced, as table numbers above, 0xFF after the last (factory 1 2 3:
//                YENDAS, IDO, REEF8)
//  The screen shows the initials, the points, the courses and a rider; where the rider
//  comes from was not followed. The service keeps `time` = 2^32 + 9999999 - points, above
//  every course time (an f32's bits, below 2^31), so the lowest key is the best; formatTime
//  tells them apart by that range.
//
// A course time's key (`time`) is the f32's bits as a u32: for positive floats they rise
// with the value, and they keep every time exactly as the game wrote it.
//
// Not played to a ranking in MAME (a bike race would need a driving script); the entries
// here are the factory ones, and an entry the game writes may set bytes the factory ones
// leave zero (kept, not checked). motoraiddx (epr-20213/20214) is not in this container's
// ROMs, so its layout is not checked.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'motoraid';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 24;
export const ENTRIES = 8;

export const BLOCK_AT = 0x1C4;          // u16 checksum, then the data it covers
export const BLOCK_SIZE = 0x484;
export const COPY_AT = 0x648;
export const TABLES_AT = 0x1C8;
export const TABLE_STRIDE = 0xC0;

export const COURSES = ['JUNOS', 'YENDAS', 'IDO', 'REEF8', 'BOWEL'];
export const TABLES = [...COURSES.map((c) => `${c} course`), 'Championship'];
const CHAMPIONSHIP = 5;
export const TABLE_ENTRIES = TABLES.map(() => ENTRIES);
export const TABLE_SCORE = TABLES.map((_, t) => t === CHAMPIONSHIP);

const MAX_SECONDS = 6000;           // 100'00"00
const MAX_POINTS = 9999999;
const SCORE_KEY = 2 ** 32;
const INITIAL = /^[\x20-\x7e]{3}$/;

const u16 = (b, at) => b[at] | b[at + 1] << 8;
const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const f32 = (bits) => new DataView(Uint32Array.of(bits).buffer).getFloat32(0, true);

/* The checksum of 0x1f310. */
export function checksum(bytes) {
  let g = 0xDEBDEC00;
  for (const b of bytes) {
    g = ((g & 0xFFFFFF00) + b) >>> 0;
    for (let k = 0; k < 8; k++) g = g > 0x7FFFFFFF ? (g * 2 + 0x10210000) >>> 0 : (g * 2) >>> 0;
  }
  return g >>> 16;
}

/* True when the block at `at` (BLOCK_AT or COPY_AT) matches its checksum. */
export function blockValid(backup, at = BLOCK_AT) {
  return u16(backup, at) === checksum(backup.subarray(at + 2, at + BLOCK_SIZE));
}

/* One 24-byte entry of table `table` -> { time, initials, ..., hex }, or null. */
export function parseEntry(bytes, table = 0) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[3] !== 0) return null;
  if (!Number.isInteger(table) || table < 0 || table >= TABLES.length) return null;
  const initials = String.fromCharCode(bytes[0], bytes[1], bytes[2]);
  if (!INITIAL.test(initials)) return null;
  const hex = bytesToHex(bytes);
  if (table === CHAMPIONSHIP) {
    const points = u32(bytes, 0xC);
    if (points === 0 || points > MAX_POINTS) return null;
    const courses = [];
    for (let i = 0x11; i < ENTRY_SIZE && bytes[i] !== 0xFF; i++) {
      if (bytes[i] >= COURSES.length) return null;
      courses.push(bytes[i]);
    }
    return { time: SCORE_KEY + MAX_POINTS - points, score: points, initials, courses, hex };
  }
  const bits = u32(bytes, 4), t = f32(bits);
  if (!(t > 0 && t < MAX_SECONDS) || bytes[8] > 3) return null;
  return { time: bits, seconds: t, initials, rider: bytes[8], hex };
}

/* The tables, from the block the game would keep (the first, else its copy; a board with
 * neither valid gets the factory tables at boot, so it has none). */
export function readTables(backup) {
  const at = blockValid(backup, BLOCK_AT) ? BLOCK_AT : blockValid(backup, COPY_AT) ? COPY_AT : -1;
  return TABLES.map((_, t) => {
    const out = [];
    if (at < 0) return out;
    for (let i = 0; i < ENTRIES; i++) {
      const p = at + (TABLES_AT - BLOCK_AT) + t * TABLE_STRIDE + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(p, p + ENTRY_SIZE), t);
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes each table (best first, 8 at most; slots past a short table keep theirs), signs
 * the block and copies it to 0x648. */
export function writeTables(backup, tables) {
  if (!blockValid(backup, BLOCK_AT) && blockValid(backup, COPY_AT)) {
    backup.copyWithin(BLOCK_AT, COPY_AT, COPY_AT + BLOCK_SIZE);   // as the boot would
  }
  TABLES.forEach((_, t) => {
    const fit = (tables[t] || []).map((e) => parseEntry(hexToBytes(e.hex), t)).filter(Boolean);
    for (let i = 0; i < ENTRIES && i < fit.length; i++) {
      backup.set(hexToBytes(fit[i].hex), TABLES_AT + t * TABLE_STRIDE + i * ENTRY_SIZE);
    }
  });
  const c = checksum(backup.subarray(BLOCK_AT + 2, BLOCK_AT + BLOCK_SIZE));
  backup[BLOCK_AT] = c & 0xFF;
  backup[BLOCK_AT + 1] = c >> 8;
  backup.copyWithin(COPY_AT, BLOCK_AT, BLOCK_AT + BLOCK_SIZE);
  return backup;
}

/* A course time's key as the game shows it (m'ss"cc), or the championship's points. */
export function formatTime(t) {
  if (t >= SCORE_KEY) return String(SCORE_KEY + MAX_POINTS - t);
  const s = f32(t), m = Math.trunc(s / 60);
  const ss = Math.trunc(s - m * 60), cc = Math.trunc((s - Math.trunc(s)) * 100);
  return `${m}'${String(ss).padStart(2, '0')}"${String(cc).padStart(2, '0')}`;
}

export function detail(e) {
  if (!e) return '';
  if (e.courses) return e.courses.map((c) => COURSES[c]).join(', ');
  return '';
}
