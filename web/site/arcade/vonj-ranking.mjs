// Cyber Troopers Virtual-On's two rankings, as the board keeps them in its backup RAM.
//
// Shared by the server and the page, like srally-ranking.mjs. m2-hle2's web build keeps the
// 16 KB of backup RAM in localStorage as hex (m2hle2.nvram.vonj). Read out of the vonj
// program (Pinboard #1091; names from the PS3 build's symbol table, rokuYAMP
// docs/von-rom-symbols.md). von, the USA program, keeps the same block with the same
// header at the same addresses, and its board is shared (games.mjs, Pinboard #1342):
//
// The game keeps one 0x20C-byte block twice, at 0 and 0x20C (BackupBKRAM 0x2330 copies the
// first over the second after every change). At boot InitBackup (0x2440) checks the first:
// the 16 bytes "SEGA.AM#3.VRON.\0", the block's size 0x20C as a u32 at +0x10, and at +0x14
// a u16 LE GetCRC (0x3120) of the settings, +0x16..+0x38. A bad first copy is replaced by
// the second; if that one fails too, the block is factory-filled (rankings included). The
// CRC covers the settings only: nothing signs the rankings, so a write needs no new CRC.
// loadRanking (0xE37F0) copies both tables to RAM (0x578410, 0x578460); saveRanking
// (0xE37B0) copies them back after a name entry.
//
//   +0x144  TODAY'S BEST PILOTS: 10 entries of 8 bytes, best first (InitNameEntry 0xE4250
//           puts a clear time in before the first slot it beats or that is empty):
//             +0  3 chars initials (A-Z 0-9 . < > | ~ {, the name entry's letters)
//             +3  u8 machine (0 Temjin .. 9 Z-Gradt, the name table at ROM 0x19360)
//             +4  u32 LE clear time, in 1/48 s as initDispRanking1 shows it (m = t/2880,
//                 s = t/48 % 60, cc = t%48 * 99/48); 0xFFFFFFFF is an empty slot ("99'99"99")
//   +0x194  TODAY'S TOP OF WINS: 10 entries of 12 bytes, best first (more wins first, then
//           fewer losses; a tie goes above the old entry):
//             +0  3 chars initials, +3 u8 machine, as above
//             +4  u32 LE wins, +8 u32 LE losses
// The factory fills both with "MSB" Temjin: 287952 (99'59"00) and 5 wins 0 losses.
// (+0xA4: 10 x 16 bytes of FAVORITE MACHINES play counts, bookkeeping, not a ranking.)
//
// The service sorts by `time`, lower first. The wins table ranks by the highest wins, so its
// entries carry a key instead, above every time: 2^32 + (0xFFFF - wins) * 0x10000 + losses
// (wins and losses up to 0xFFFF; the game shows at most 999). formatTime tells the two apart
// by that range. An entry's size says its table: 8 bytes the times, 12 the wins.

import { hexToBytes, bytesToHex } from './ranking.mjs';

export const BACKUP_SIZE = 0x4000;
export const SET = 'vonj';
export const NVRAM_KEY = 'm2hle2.nvram.vonj';
export const TABLES = ['Best pilots', 'Most wins'];
export const ENTRY_SIZES = [8, 12];
export const ENTRY_SIZE = 12;   // the larger; the times table's entries are 8 bytes
export const ENTRIES = 10;
export const COPY_SIZE = 0x20C;
export const COPIES = [0, COPY_SIZE];

const TABLE_AT = [0x144, 0x194];
const HEADER = 'SEGA.AM#3.VRON.\0';
const WINS_KEY = 2 ** 32;
const INITIAL = /^[A-Z0-9.<>|~{]{3}$/;
const MACHINES = ['Temjin', 'Viper II', 'Belgdor', 'Raiden', 'Dorkas', 'Fei-Yen', 'Apharmd',
  'Bal-Bas-Bow', 'Jaguarandi', 'Z-Gradt'];

const u32 = (b, at) => (b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) >>> 0;
const u16 = (b, at) => b[at] | b[at + 1] << 8;

const CCITT = Array.from({ length: 256 }, (_, i) => {
  let c = i << 8;
  for (let k = 0; k < 8; k++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
  return c & 0xFFFF;
});

/* GetCRC (0x3120): the ROM's CRC-CCITT table at 0x2F20, seeded 0xDEBDEB00. */
export function getCrc(bytes) {
  let c = 0xDEBDEB00;
  for (const x of bytes) c = ((((c + x) >>> 0) << 8) ^ (CCITT[c >>> 24] << 16)) >>> 0;
  return (CCITT[c >>> 24] ^ (c >>> 8)) & 0xFFFF;
}

/* True when a copy passes InitBackup's check: header, size, settings CRC. */
export function copyValid(backup, copy) {
  for (let i = 0; i < 16; i++) if (backup[copy + i] !== HEADER.charCodeAt(i)) return false;
  return u32(backup, copy + 0x10) === COPY_SIZE
    && u16(backup, copy + 0x14) === getCrc(backup.subarray(copy + 0x16, copy + 0x38));
}

/* One entry (8 bytes: a time; 12 bytes: wins) -> { time, initials, machine, hex, ... } or null;
 * also null when `table` (the server passes the one it was sent for) is not its size's. */
export function parseEntry(bytes, table) {
  if (!bytes || (bytes.length !== 8 && bytes.length !== 12)) return null;
  if (table !== undefined && bytes.length !== ENTRY_SIZES[table]) return null;
  const initials = String.fromCharCode(bytes[0], bytes[1], bytes[2]), machine = bytes[3];
  if (!INITIAL.test(initials) || machine >= MACHINES.length) return null;
  const hex = bytesToHex(bytes);
  if (bytes.length === 8) {
    const time = u32(bytes, 4);
    if (time === 0 || time === 0xFFFFFFFF) return null;
    return { table: 0, time, initials, machine, hex };
  }
  const wins = u32(bytes, 4), losses = u32(bytes, 8);
  if (wins > 0xFFFF || losses > 0xFFFF) return null;
  return { table: 1, time: WINS_KEY + (0xFFFF - wins) * 0x10000 + losses, wins, losses, initials,
    machine, hex };
}

/* Both tables, from the copy the game would keep (the first, else the second). */
export function readTables(backup) {
  const copy = COPIES.find((c) => copyValid(backup, c)) ?? 0;
  return TABLES.map((_, t) => {
    const out = [], size = ENTRY_SIZES[t];
    for (let i = 0; i < ENTRIES; i++) {
      const at = copy + TABLE_AT[t] + i * size;
      const e = parseEntry(backup.subarray(at, at + size));
      if (e) out.push(e);
    }
    return out;
  });
}

/* Writes the tables (best first, 10 each) into both copies. An entry of the other table's
 * size is skipped. No CRC covers the rankings. */
export function writeTables(backup, tables) {
  for (const copy of COPIES) {
    tables.forEach((table, t) => {
      const size = ENTRY_SIZES[t];
      if (!size) return;
      const fit = table.map((e) => hexToBytes(e.hex)).filter((b) => b.length === size);
      for (let i = 0; i < ENTRIES && i < fit.length; i++) backup.set(fit[i], copy + TABLE_AT[t] + i * size);
    });
  }
  return backup;
}

/* A time as the game shows it (mm'ss"cc, 48 ticks a second, minutes up to 99), or a
 * wins key as "N wins M losses". */
export function formatTime(t) {
  if (t >= WINS_KEY) {
    const k = t - WINS_KEY, wins = 0xFFFF - Math.floor(k / 0x10000), losses = k % 0x10000;
    return `${wins} win${wins === 1 ? '' : 's'} ${losses} loss${losses === 1 ? '' : 'es'}`;
  }
  const m = Math.min(99, Math.floor(t / 2880)), s = Math.floor(t / 48) % 60;
  const cs = Math.floor((t % 48) * 99 / 48);
  return `${String(m).padStart(2, '0')}'${String(s).padStart(2, '0')}"${String(cs).padStart(2, '0')}`;
}

export function machineName(m) { return MACHINES[m] || ''; }

export function detail(e) { return e ? machineName(e.machine) : ''; }
