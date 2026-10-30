// Wave Runner's rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps
// the 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.waverunr). Read out of the
// waverunr (Revision A) program, the only set there is (Pinboard #1911).
//
// The game keeps two copies of a 0xE14-byte block: A at 0x10C, B at 0xF20. A copy is a
// u16 LE checksum, then 0xE12 bytes: a u16 (0) and the three course rankings. The
// checksum is the program's CRC (0x2c080): hotd's 16-bit CRC in a 32-bit register,
// seeded 0xDEBDEC00 rather than 0xDEBDEB00, without the eight closing shifts (crc16
// below). The settings at 0x8 (u16 checksum, then 0x72 bytes, copy at 0x7C) are signed
// the same way. At boot (0xadf0): a good A is signed again and copied over a B that
// differs (0xaf40, 0xb090); a bad A is replaced by a good B (0xb110); two bad copies get
// the factory rankings (0xaf10 -> 0x4e880), signed in A and copied to B.
//
// Three rankings, one per course, 100 entries of 12 bytes at copy + 4 + 0x4B0 * course,
// best first. The attract mode shows the first (初級, beginner) as "RANKING" 1 to 100;
// the other two are taken to be 中級 and 上級 in that order (not seen on screen).
// 0x4ebb0 ranks a time only below entry 99's (copy + 4 + 0x4AC) and shifts the rest down.
//   +0   3 bytes  initials, a code each (code % 40, glyphs from ROM 0x4ade0): 0-25 A-Z,
//                26 '.', 27 '!', 28 '?', 29 space, 30-39 '0'-'9' (seen in the attract
//                ranking: "O.O", "H.0", "A91", "!?.")
//   +3   u8   (r9 << 1) | flag, 0 in the factory board
//   +4   3 bytes  the MARK column: an emblem each (per-course glyph tables, ROM 0x4e090);
//                the factory ones are 0x46 + rand % 40 (0x46 in MAME)
//   +7   u8   0xFF, or 0 (the factory fill: entry i with (i + 11) % 34 == 0)
//   +8   f32 LE  the time in seconds, LOWER wins; shown m'ss"cc (2'41"02 = 161.02)
// The factory times (0x4e880) are 161.02, 181.61 and 200.73 (ROM 0x4e860) plus
// 0.1612 * the rank; with the race setting at RAM 0x5132f0 set, 416.023, 462.13 and
// 523.35 plus 0.2926 * the rank. The factory initials are 300 x 3 codes at ROM 0x4e4d0.
//
// The service keeps `time` in milliseconds (the float rounded); the bytes are the game's.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'waverunr';
export const NVRAM_KEY = `m2hle2.nvram.${SET}`;
export const ENTRY_SIZE = 12;
export const ENTRIES = 100;
export const COPY_SIZE = 0xE14;
export const COPIES = [0x10C, 0xF20];
export const TABLE_SIZE = ENTRIES * ENTRY_SIZE;   // 0x4B0
export const TABLES = ['Beginner', 'Intermediate', 'Advanced'];
export const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ.!? 0123456789';   // code n is CHARS[n]

const MAX_TIME = 600;   // seconds: the ranking shows one digit of minutes

/* The program's checksum (0x2c080): poly 0x1021 in the top half of a 32-bit register. */
export function crc16(bytes) {
  let g = 0xDEBDEC00;
  for (const x of bytes) {
    g = ((g & 0xFFFFFF00) >>> 0) + x;
    for (let i = 0; i < 8; i++) g = g > 0x7FFFFFFF ? ((g << 1) + 0x10210000) >>> 0 : (g << 1) >>> 0;
  }
  return g >>> 16;
}

const u16 = (b, at) => b[at] | b[at + 1] << 8;

export function copyValid(backup, copy) {
  return u16(backup, copy) === crc16(backup.subarray(copy + 2, copy + COPY_SIZE));
}

/* One 12-byte entry -> { time, seconds, initials, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  let initials = '';
  for (let i = 0; i < 3; i++) {
    if (bytes[i] >= CHARS.length) return null;
    initials += CHARS[bytes[i]];
  }
  const seconds = new DataView(bytes.buffer, bytes.byteOffset, ENTRY_SIZE).getFloat32(8, true);
  if (!Number.isFinite(seconds) || seconds <= 0 || seconds >= MAX_TIME) return null;
  return { time: Math.round(seconds * 1000), seconds, initials, hex: bytesToHex(bytes) };
}

/* The three rankings, from the first good copy (from A if neither is good). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  return TABLES.map((_, t) => {
    const out = [];
    for (let i = 0; i < ENTRIES; i++) {
      const at = copy + 4 + t * TABLE_SIZE + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes each ranking (best first, 100 at most) into the good copy, signs it and copies
 * it over the other, as the game does after a name entry. */
export function writeTables(backup, tables) {
  const [a, b] = COPIES;
  const from = !copyValid(backup, a) && copyValid(backup, b) ? b : a, to = from === a ? b : a;
  TABLES.forEach((_, t) => {
    const table = tables[t] || [];
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      backup.set(hexToBytes(table[i].hex), from + 4 + t * TABLE_SIZE + i * ENTRY_SIZE);
    }
  });
  const c = crc16(backup.subarray(from + 2, from + COPY_SIZE));
  backup.set([c & 0xFF, c >> 8], from);
  backup.copyWithin(to, from, from + COPY_SIZE);
  return backup;
}

/* A time in milliseconds as the game shows it: m'ss"cc. */
export function formatTime(t) {
  const cs = Math.floor(t / 10) % 100, s = Math.floor(t / 1000) % 60, m = Math.floor(t / 60000);
  return `${m}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}
