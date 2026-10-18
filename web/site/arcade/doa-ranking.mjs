// Dead or Alive's best clear times, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps the
// 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.doa). Read out of the doa program
// (Model 2B Rev C; Pinboard #1091):
//
// The game keeps one 0x344-byte block twice, at 0x008 and 0x34C. Each copy is led by a u16
// LE checksum: the plain byte sum (0x70d0), mod 0x10000, of the copy's next 0x342 bytes
// ([+0x002, +0x344)). The boot (0x5f60) checks the first: if it is good and the second's
// checksum word differs, the first is copied over the second; if it is bad and the second
// sums right, the second is copied back; if both are bad, 0x000-0x68F is cleared and the
// factory times (ROM 0x904c0) go in. After a change (0x6020) the first is re-summed and
// copied over the second. A time above 359999 (99'59"59) makes the boot (0x4890) stop on an
// error screen, so such a slot is never an entry. Outside the block: a u32 at 0x000, the
// seconds the board has been on (test menu bookkeeping, not summed).
//
// A copy holds the test menu's bookkeeping, then from +0x254 (0x25C in the first copy) the
// two rankings, 10 entries of 12 bytes each, best (lowest) time first:
//   +0  u32 LE  clear time in frames (1/60 s): m = t / 3600, s = t / 60 % 60,
//               hundredths = t % 60 * 100 / 60 (the display, 0x3d640)
//   +4  u16 LE  character (0x54fc01; the ranking screen shows id & 0xF, 0x3d7c0)
//   +6  3 x u16 LE initials, one character each (the name entry's "A-Z . 0-9 ! ?")
// The first table is the normal game; the second is BURST MODE's (the test menu setting,
// then Start with all three buttons held: 0x4840), as the insert (0x1ea60) and the ranking
// screen pick them. The factory fills both with 10'00"00 to 19'00"00 (AKU ... NOB, EHA ... Y.H).
// Lower time wins, as the service sorts.
//
// doab, doaa, doaab and doaae write the same factory block, with the same routines.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'doa';
export const NVRAM_KEY = 'm2hle2.nvram.doa';
export const ENTRY_SIZE = 12;
export const ENTRIES = 10;
export const COPY_SIZE = 0x344;
export const COPIES = [0x008, 0x008 + COPY_SIZE];
export const TABLES = ['Normal', 'Burst mode'];

const RANKING_IN_COPY = 0x254, MAX_TIME = 359999;
// The character ids as the select screen sets them (Raidou, the boss, by elimination).
const CHARACTERS = { 0: 'Zack', 1: 'Tina', 2: 'Jann Lee', 4: 'Ryu', 5: 'Kasumi', 6: 'Gen Fu',
  8: 'Bayman', 11: 'Raidou', 12: 'Lei Fang' };
const LETTER = /^[A-Z0-9.,!? ]$/;

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const u16 = (b, at) => b[at] | b[at + 1] << 8;

export function checksum(bytes) {
  let s = 0;
  for (const x of bytes) s += x;
  return s & 0xFFFF;
}

/* One 12-byte entry -> { time, initials, character, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE) return null;
  const time = u32(bytes, 0), character = u16(bytes, 4);
  if (time === 0 || time > MAX_TIME || character > 0xFF || !CHARACTERS[character & 0xF]) return null;
  let initials = '';
  for (let i = 0; i < 3; i++) {
    const c = u16(bytes, 6 + 2 * i);
    if (c > 0x7F || !LETTER.test(String.fromCharCode(c))) return null;
    initials += String.fromCharCode(c);
  }
  return { time, initials, character, hex: bytesToHex(bytes) };
}

/* True when a copy's checksum word is its bytes' sum, as the game checks it. */
export function copyValid(backup, copy) {
  return u16(backup, copy) === checksum(backup.subarray(copy + 2, copy + COPY_SIZE));
}

const tableAt = (copy, t) => copy + RANKING_IN_COPY + t * ENTRIES * ENTRY_SIZE;

/* The two tables, from the copy the game would keep (the first that checks out). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? COPIES[0];
  return TABLES.map((_, t) => {
    const out = [];
    for (let i = 0; i < ENTRIES; i++) {
      const at = tableAt(copy, t) + i * ENTRY_SIZE;
      const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 10 each) into both copies and sums each copy again. */
export function writeTables(backup, tables) {
  for (const copy of COPIES) {
    tables.forEach((table, t) => {
      for (let i = 0; i < ENTRIES && i < table.length; i++) {
        backup.set(hexToBytes(table[i].hex), tableAt(copy, t) + i * ENTRY_SIZE);
      }
    });
    const s = checksum(backup.subarray(copy + 2, copy + COPY_SIZE));
    backup[copy] = s & 0xFF;
    backup[copy + 1] = s >> 8;
  }
  return backup;
}

/* mm'ss"cc, as the ranking screen shows it (minutes in two digits, at most 99). */
export function formatTime(frames) {
  const m = Math.min(99, Math.floor(frames / 3600)), s = Math.floor(frames / 60) % 60;
  const cs = Math.floor((frames % 60) * 100 / 60);
  return `${String(m).padStart(2, '0')}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}

export function characterName(c) { return CHARACTERS[c & 0xF] || ''; }

export function detail(e) { return e ? characterName(e.character) : ''; }
