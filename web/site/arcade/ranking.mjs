// Sonic the Fighters' ranking, as the board keeps it in its backup RAM.
//
// Shared by the server and the page (served as /ranking.mjs), so both read and
// write the table the same way. The backup RAM is the 16 KB at 0x01D00000, MAME's
// nvram/sfight/backup1 byte for byte; m2-hle2's web build keeps it in localStorage
// as hex (key m2hle2.nvram.sfight, src/core/backup_ram.h).
//
// The ranking is 8 entries of 16 bytes at 0x3100, best first:
//   +0  u32 LE  time: lower is better; it reads as hundredths of a second
//               (the factory names run 3000.00 s up, FLY's clear is 438.34 s)
//   +4  3 chars initials, then a NUL
//   +8  8 bytes the game's own (character, stage...), kept as they are

export const BACKUP_SIZE = 0x4000;
export const RANKING_AT = 0x3100;
export const ENTRY_SIZE = 16;
export const ENTRIES = 8;
export const NVRAM_KEY = 'm2hle2.nvram.sfight';

const INITIAL = /^[\x20-\x5f]{3}$/;     // what the name entry offers: A-Z, digits, marks

export function hexToBytes(hex) {
  if (typeof hex !== 'string' || hex.length % 2 || /[^0-9a-f]/i.test(hex)) return null;
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

export function bytesToHex(bytes) {
  let s = '';
  for (const b of bytes) s += (b | 256).toString(16).slice(1);
  return s;
}

/* One 16-byte entry -> { time, initials, hex }, or null if it is not one. */
export function parseEntry(bytes) {
  if (!bytes || bytes.length !== ENTRY_SIZE || bytes[7] !== 0) return null;
  const time = (bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24) >>> 0;
  const initials = String.fromCharCode(bytes[4], bytes[5], bytes[6]);
  if (!INITIAL.test(initials) || time === 0) return null;
  return { time, initials, hex: bytesToHex(bytes) };
}

/* The table in a backup RAM image (16 KB of bytes). Entries that do not parse are left out. */
export function readRanking(backup) {
  const out = [];
  for (let i = 0; i < ENTRIES; i++) {
    const at = RANKING_AT + i * ENTRY_SIZE;
    const e = parseEntry(backup.subarray(at, at + ENTRY_SIZE));
    if (e) out.push(e);
  }
  return out;
}

/* Writes a table (best first, at most 8) back into a backup RAM image. */
export function writeRanking(backup, entries) {
  for (let i = 0; i < ENTRIES && i < entries.length; i++) {
    backup.set(hexToBytes(entries[i].hex), RANKING_AT + i * ENTRY_SIZE);
  }
  return backup;
}

/* The best 8 (or `size`) of both tables. The same entry (all its bytes) counts once;
 * equal times keep the one that was on the table first. */
export function mergeRanking(table, incoming, size = ENTRIES) {
  const seen = new Set(table.map((e) => e.hex));
  const all = table.slice();
  for (const e of incoming) {
    if (!seen.has(e.hex)) { seen.add(e.hex); all.push(e); }
  }
  all.sort((a, b) => a.time - b.time);
  return all.slice(0, size);
}

/* Entries in `mine` that `base` does not have: what a player set since loading the board. */
export function newEntries(base, mine) {
  const known = new Set(base.map((e) => e.hex));
  return mine.filter((e) => !known.has(e.hex));
}

export function formatTime(time) {
  const cs = time % 100, s = Math.floor(time / 100) % 60, m = Math.floor(time / 6000);
  return `${m}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}
