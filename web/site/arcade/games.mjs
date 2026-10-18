// The games the arcade keeps a board for, each behind the same face, so the server and
// the page treat them alike:
//
//   id, title, nvramKey, backupSize, entrySize, entries (per table), tables (names),
//   parseEntry(bytes), readTables(backup), writeTables(backup, tables), formatTime(t),
//   detail(entry)  anything shown beside the time (Sega Rally's car)
//   score          true when the game ranks by points: `time` is then a key that sorts the
//                  same way (lower wins), formatTime shows the points
//   sets           { profile: ROM set } for a game m2-hle2 runs as several profiles: the
//                  board goes under each listed profile's own key (m2hle2.nvram.<set>), and
//                  a profile not listed (another layout, or not checked) plays alone
//
// Sonic the Fighters has one table, Sega Rally six. A missing id is Sonic the Fighters, as
// every request was before there were two; an unknown one is no game.

import * as stf from './ranking.mjs';
import * as rally from './srally-ranking.mjs';
import * as vonj from './vonj-ranking.mjs';
import * as dynamcop from './dynamcop-ranking.mjs';
import * as fvipers from './fvipers-ranking.mjs';
import * as hotd from './hotd-ranking.mjs';
import * as daytona from './daytona-ranking.mjs';
import * as vf2 from './vf2-ranking.mjs';
import * as doa from './doa-ranking.mjs';
import * as airwlkrs from './airwlkrs-ranking.mjs';

const GAMES = {
  sfight: {
    id: 'sfight', title: 'Sonic the Fighters', nvramKey: stf.NVRAM_KEY, backupSize: stf.BACKUP_SIZE,
    entrySize: stf.ENTRY_SIZE, entries: stf.ENTRIES, tables: ['Ranking'],
    parseEntry: stf.parseEntry, formatTime: stf.formatTime, detail: () => '',
    readTables: (b) => [stf.readRanking(b)],
    writeTables: (b, tables) => stf.writeRanking(b, tables[0]),
  },
  srallyc: {
    id: 'srallyc', title: 'Sega Rally Championship', nvramKey: rally.NVRAM_KEY,
    backupSize: rally.BACKUP_SIZE, entrySize: rally.ENTRY_SIZE, entries: rally.ENTRIES,
    tables: rally.TABLES, parseEntry: rally.parseEntry, formatTime: rally.formatTime,
    detail: (e) => rally.carName(e.car), readTables: rally.readTables, writeTables: rally.writeTables,
  },
};

/* Each profile's set is its id (m2-hle2's profile_rom_set). */
const same = (...ids) => Object.fromEntries(ids.map((id) => [id, id]));

/* Pinboard #1091: every other game the arcade site runs, each read out of its program
 * (the header of its module says where and how). Only the sets that were seen to keep the
 * same layout share a board; vf2a and vf2o check another version and would wipe it, and
 * fvipers, hotd, daytona and airwlkrs have one profile each. */
const MORE = [
  ['vonj', 'Cyber Troopers Virtual-On', vonj, { sets: same('vonj', 'vonu') }],
  ['dynamcop', 'Dynamite Cop', dynamcop,
    { sets: same('dynamcopb', 'dyndeka2b', 'dynamcop', 'dyndeka2', 'dynamcopc') }],
  ['fvipers', 'Fighting Vipers', fvipers, { sets: same('fvipers', 'fvipersb') }],
  ['hotd', 'The House of the Dead', hotd, { sets: same('hotd') }],
  ['daytona', 'Daytona USA', daytona, { sets: same('daytona') }],
  ['vf2', 'Virtua Fighter 2', vf2, { sets: same('vf2', 'vf2b') }],
  ['doa', 'Dead or Alive', doa, { sets: same('doa', 'doab', 'doaa', 'doaab', 'doaae') }],
  ['airwlkrs', 'Air Walkers', airwlkrs, { sets: same('airwlkrs') }],
];
for (const [id, title, m, extra] of MORE) GAMES[id] = fromModule(id, title, m, extra);

/* A game whose ranking module (<id>-ranking.mjs) has the usual face: Sega Rally's names. */
function fromModule(id, title, m, extra = {}) {
  return {
    id, title, nvramKey: m.NVRAM_KEY, backupSize: m.BACKUP_SIZE, entrySize: m.ENTRY_SIZE,
    entries: m.ENTRIES, tables: m.TABLES, parseEntry: m.parseEntry, formatTime: m.formatTime,
    detail: m.detail || (() => ''), readTables: m.readTables, writeTables: m.writeTables,
    score: !!m.SCORE, ...extra,
  };
}

export const GAME_IDS = Object.keys(GAMES);

export function gameFor(id) { return GAMES[id] || (id ? null : GAMES.sfight); }

/* Where the board goes in localStorage for a profile of the game; null when that profile
 * does not share the board (no profile: the game's default). */
export function nvramKeyFor(game, profile) {
  if (!profile || !game.sets) return game.nvramKey;
  return game.sets[profile] ? `m2hle2.nvram.${game.sets[profile]}` : null;
}
