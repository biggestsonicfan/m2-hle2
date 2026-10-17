// The games the arcade keeps a board for, each behind the same face, so the server and
// the page treat them alike:
//
//   id, title, nvramKey, backupSize, entrySize, entries (per table), tables (names),
//   parseEntry(bytes), readTables(backup), writeTables(backup, tables), formatTime(t),
//   detail(entry)  anything shown beside the time (Sega Rally's car)
//
// Sonic the Fighters has one table, Sega Rally six. A missing or unknown id is
// Sonic the Fighters, as every request was before there were two.

import * as stf from './ranking.mjs';
import * as rally from './srally-ranking.mjs';

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

export const GAME_IDS = Object.keys(GAMES);

export function gameFor(id) { return GAMES[id] || (id ? null : GAMES.sfight); }
