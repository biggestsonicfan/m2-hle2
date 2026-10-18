// Daytona USA's best race times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps the
// 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.daytona). Read out of the daytona
// program (Japan Rev A; Pinboard #1091):
//
// Three tables of 24 entries, 24 bytes each, one per course, one copy each and no checksum:
// 0x360 the beginner course (777 Speedway), 0x5A0 the expert (Sea-Side Street Galaxy) and 0x7E0
// the advanced (Dinosaur Canyon), as the ranking screen titles them (the course number picks
// one from 0x2390e4). The boot (0x5a98) checks only the "S-RAM CHECK OK (4.0)" at
// 0x200; without it the board is set up from scratch. It then compares the race length setting
// (backup 0x01C: normal, grand prix, endurance) with the one the tables were made for (0xBA0),
// and on a difference fills all three again (0x2270d0) with that length's factory times. An
// entry, best (lowest) time first:
//   +0x00 u32 LE  place, 1-24 (the slot's own number; the insert keeps it, 0x227508, and so
//                 does writeTables)
//   +0x04 u32 LE  race time in 1/64 s (the ranking screen, 0x19124: hundredths = t * 100 / 64)
//   +0x08 u32 LE  0-3, which of four counters (0x501700) ran highest in the race
//   +0x0C u32 LE  two flags: bit 0 (0x501a80) puts a "T.A." mark beside the time on the ranking
//                 screen, bit 1 (0x501a90) a second, smaller mark (0x2272e4)
//   +0x10 u32 LE  the race time again (the one the screen shows)
//   +0x14 char[4] initials, NUL-terminated: up to three of "A-Z ." (0x239104; the factory's
//                 AM2 has a digit too); "   " for a place below the name entry's top five,
//                 nothing if it timed out first
// A race (0x227408) goes in when it beats place 24, then bubbles up; only the top five are on
// the ranking screen. The factory (normal length) gives each course five names (NAG GOS YAS GOM
// KAG, YU. ARI OSA MAC KEN, MAS MIT DEK D.K MIY) and AM2 for the rest, from 2'40"00 (beginner)
// and 4'00"00 (the others) in 5-second steps. Lower time wins, as the service sorts.
//
// Other bytes, left alone here: two copies of the settings at 0x000 and 0x080 and of a smaller
// block at 0x100 and 0x180, each with its own sum, and at 0xA20 a pointer per course to the
// entry the last race made (for its name entry).

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'daytona';
export const NVRAM_KEY = 'm2hle2.nvram.daytona';
export const ENTRY_SIZE = 24;
export const ENTRIES = 24;
export const TABLE_OFFSETS = [0x360, 0x5A0, 0x7E0];
export const TABLES = ['Beginner', 'Expert', 'Advanced'];

// 100 minutes: no race at any length gets near it.
const MAX_TIME = 100 * 60 * 64;
const LETTER = /^[A-Z0-9. ]$/;

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;

/* One 24-byte entry -> { time, initials, place, counter, flags, hex }, or null if it is not one.
   hex is the entry with its place cleared, so a race keeps one hex at whatever place it sits;
   that form (place 0, as the page sends it) parses too. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const place = u32(bytes, 0), time = u32(bytes, 4), counter = u32(bytes, 8), flags = u32(bytes, 12);
  if (place > ENTRIES || time === 0 || time >= MAX_TIME || u32(bytes, 16) !== time) return null;
  if (counter > 3 || flags > 3 || bytes[23] !== 0) return null;
  let initials = '', end = false;
  for (let i = 20; i < 23; i++) {
    const c = bytes[i];
    if (c === 0) { end = true; continue; }
    if (end || !LETTER.test(String.fromCharCode(c))) return null;
    initials += String.fromCharCode(c);
  }
  const canon = bytes.slice();
  canon.fill(0, 0, 4);
  return { time, initials, place, counter, flags, hex: bytesToHex(canon) };
}

/* The three tables, each best first. */
export function readTables(backup) {
  return TABLE_OFFSETS.map((base) => {
    const out = [];
    for (let i = 0; i < ENTRIES; i++) {
      const at = base + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 24 each), each entry with its slot's place. Nothing is summed. */
export function writeTables(backup, tables) {
  tables.forEach((table, t) => {
    for (let i = 0; i < ENTRIES && i < table.length; i++) {
      const at = TABLE_OFFSETS[t] + i * ENTRY_SIZE;
      backup.set(hexToBytes(table[i].hex), at);
      backup[at] = i + 1;
      backup[at + 1] = backup[at + 2] = backup[at + 3] = 0;
    }
  });
  return backup;
}

/* m'ss"cc, as the ranking screen shows it. */
export function formatTime(t) {
  const cs = Math.floor(t * 100 / 64);
  const m = Math.floor(cs / 6000), s = Math.floor(cs / 100) % 60, c = cs % 100;
  return `${m}'${String(s).padStart(2, '0')}"${String(c).padStart(2, '0')}`;
}

/* The ranking screen's mark beside the time. */
export function detail(e) { return e && e.flags & 1 ? 'T.A.' : ''; }
