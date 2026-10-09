/* Replays of online matches (core/replay.h, Pinboard #572).
 *
 * With "Save a replay of every online match" on, each match this board plays on
 * RPCN or GGPO ends as a .m2replay file (a zip: the board's state at the match's cold
 * boot, both players' inputs, and replay.json, its label). The board leaves it in
 * a slot; this script takes it, keeps it in the browser (IndexedDB) and, if asked,
 * downloads it too. PLAYBACK opens a file from the computer, or one kept here,
 * shows its label and asks before it plays: a replay takes the board over until
 * Stop, which boots the player's own game again.
 *
 * Player names come from other people, so nothing here goes in as HTML. */
'use strict';

const replay = {
  M: null,
  db: null,
  name: '',          /* the open replay's file name */
  bytes: null,       /* its bytes, so "Keep it here" can store a file that came from disk */
  seeking: false,    /* the time line is being dragged */
  timer: 0,
};
const REPLAY_DB = 'm2hle-replays', REPLAY_STORE = 'replays';
const REPLAY_POLL_MS = 500;

const $r = (id) => document.getElementById(id);

/* ---- The browser's copy (IndexedDB) ----------------------------------------- */

function replayDb() {
  if (replay.db) return Promise.resolve(replay.db);
  return new Promise((ok, fail) => {
    const rq = indexedDB.open(REPLAY_DB, 1);
    rq.onupgradeneeded = () => rq.result.createObjectStore(REPLAY_STORE, { keyPath: 'name' });
    rq.onsuccess = () => { replay.db = rq.result; ok(rq.result); };
    rq.onerror = () => fail(rq.error);
  });
}

function replayTx(mode, fn) {
  return replayDb().then((db) => new Promise((ok, fail) => {
    const tx = db.transaction(REPLAY_STORE, mode);
    const rq = fn(tx.objectStore(REPLAY_STORE));
    tx.oncomplete = () => ok(rq && rq.result);
    tx.onerror = () => fail(tx.error);
  }));
}
const replayPut = (rec) => replayTx('readwrite', (s) => s.put(rec));
const replayDel = (name) => replayTx('readwrite', (s) => s.delete(name));
const replayGet = (name) => replayTx('readonly', (s) => s.get(name));
const replayAll = () => replayTx('readonly', (s) => s.getAll());

/* replay.json from a zip's bytes, without the board: the list shows labels of
 * files that are not open. The sizes are read from the central directory,
 * because the writer streams its entries and leaves the local headers' at 0. */
function replayZipEntry(bytes, want) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let end = bytes.length - 22;
  while (end >= 0 && dv.getUint32(end, true) !== 0x06054b50) end--;
  if (end < 0) return null;
  const count = dv.getUint16(end + 10, true);
  for (let i = 0, p = dv.getUint32(end + 16, true); i < count && p + 46 <= bytes.length; i++) {
    if (dv.getUint32(p, true) !== 0x02014b50) return null;
    const nlen = dv.getUint16(p + 28, true);
    const name = new TextDecoder().decode(bytes.subarray(p + 46, p + 46 + nlen));
    if (name === want) {
      const at = dv.getUint32(p + 42, true), size = dv.getUint32(p + 20, true);
      const skip = 30 + dv.getUint16(at + 26, true) + dv.getUint16(at + 28, true);
      return { method: dv.getUint16(p + 10, true), data: bytes.subarray(at + skip, at + skip + size) };
    }
    p += 46 + nlen + dv.getUint16(p + 30, true) + dv.getUint16(p + 32, true);
  }
  return null;
}

async function replayLabelOf(bytes) {
  const e = replayZipEntry(bytes, 'replay.json');
  if (!e) return null;
  if (e.method === 0) return JSON.parse(new TextDecoder().decode(e.data));
  if (e.method !== 8 || !window.DecompressionStream) return null;
  const s = new Blob([e.data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return JSON.parse(await new Response(s).text());
}

/* ---- Text ------------------------------------------------------------------- */

const replayClock = (frames) => {
  const s = Math.floor(frames / 60);
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
};

function replayHeadline(j) {
  const c1 = j.p1_character || '?', c2 = j.p2_character || '?';
  return (j.p1 || '?') + ' (' + c1 + ')  vs  ' + (j.p2 || '?') + ' (' + c2 + ')';
}

function replayResult(j) {
  if (j.winner === 0 || j.winner === 1) {
    const w = j.winner === 0 ? j.p1_rounds : j.p2_rounds, l = j.winner === 0 ? j.p2_rounds : j.p1_rounds;
    return j.winner_name + ' beat ' + j.loser_name + ', ' + (w | 0) + '-' + (l | 0);
  }
  return 'No result: ' + (j.ended || 'it ended early');
}

const replayWhen = (j) => (j.timestamp ? new Date(j.timestamp * 1000).toLocaleString() : '?');

function replaySay(text) { $r('replay-status').textContent = text || ''; }

/* ---- The board -------------------------------------------------------------- */

const replayStatus = () => JSON.parse(replay.M.UTF8ToString(replay.M._web_replay_status()));
const replayError = () => replay.M.UTF8ToString(replay.M._web_replay_error());

/* Hand a file to the board and show its label. */
function replayOpen(name, bytes) {
  const M = replay.M;
  if (!M || !M._web_replay_open) return;
  const st = replayStatus();
  if (st.on) { replaySay('A replay is playing: Stop it first.'); return; }
  const ptr = M._malloc(bytes.length || 1);
  M.HEAPU8.set(bytes, ptr);                      /* the board frees it */
  if (M._web_replay_open(ptr, bytes.length) !== 0) {
    replaySay(name + ': ' + replayError());
    $r('replay-label').hidden = true;
    return;
  }
  replay.name = name;
  replay.bytes = bytes;
  replaySay('');
  replayShowLabel();
}

function replayShowLabel() {
  const info = replay.M.UTF8ToString(replay.M._web_replay_info());
  if (!info) { $r('replay-label').hidden = true; return; }
  const j = JSON.parse(info);
  $r('replay-file-name').textContent = replay.name;
  $r('replay-who').textContent = replayHeadline(j);
  $r('replay-stage').textContent = 'Stage: ' + (j.stage || '?');
  $r('replay-when').textContent = 'Played ' + replayWhen(j) + ', ' + replayClock(j.frames || 0) + ' long';
  $r('replay-by').textContent = 'recorded by ' + (j.recorded_by || '?') + (j.netcode === 'ggpo' ? ' over GGPO' : '')
    + '; ' + j.romset + ', ' + j.profile;
  $r('replay-result').textContent = replayResult(j);
  $r('replay-label').hidden = false;
  replayRefresh();
}

function replayPlay() {
  if (replay.M._web_replay_play() !== 0) { replaySay(replayError()); return; }
  replay.M._web_replay_fast($r('replay-fast').checked ? 1 : 0);
  replaySay('');
  replayRefresh();
  $r('canvas').focus();
}

function replayStop() {
  replay.M._web_replay_stop();
  replaySay('Stopped: your own game is back, from power-on.');
  replayRefresh();
}

/* What the panel shows follows the board: the prompt, the controls, or why not. */
/* The REPLAY section: the switch, and whether a match is being recorded. */
function replayRefreshRecord(st) {
  $r('replay-record').checked = !!st.record;
  $r('replay-recording').hidden = !st.recording;
  $r('replay-rec-error').textContent = st.rec_error || '';
}

function replayRefresh() {
  if (!replay.M || !replay.M._web_replay_status) return;
  const st = replayStatus();
  replayRefreshRecord(st);
  $r('replay-result').hidden = $r('replay-hide').checked;
  const open = st.loaded && !$r('replay-label').hidden;
  $r('replay-ask').hidden = !open || st.on || !!st.unplayable;
  $r('replay-unplayable').hidden = !open || st.on || !st.unplayable;
  $r('replay-unplayable').textContent = st.unplayable ? 'Cannot play here: ' + st.unplayable : '';
  $r('replay-controls').hidden = !st.on;
  if (st.on) replayRefreshControls(st);
}

/* The time line and the end line, while a replay plays. */
function replayRefreshControls(st) {
  const seek = $r('replay-seek');
  seek.max = String(st.frames || 1);
  if (!replay.seeking) seek.value = String(st.frame);
  $r('replay-time').textContent = replayClock(st.frame) + ' / ' + replayClock(st.frames);
  $r('replay-fast').checked = !!st.fast;
  $r('replay-end').textContent = st.split ? 'The replay went off its record: ' + st.why
    : st.ended ? 'The end. Restart, drag back, or Stop to go back to your game.'
    : st.seeking ? 'Seeking...' : '';
}

/* ---- A finished match ------------------------------------------------------- */

async function replayTakeReady() {
  const M = replay.M, len = M._web_replay_ready_len();
  if (!len) return;
  const ptr = M._web_replay_ready_data();
  const bytes = new Uint8Array(M.HEAPU8.subarray(ptr, ptr + len));   /* a copy: the board frees its own */
  const name = M.UTF8ToString(M._web_replay_ready_name());
  M._web_replay_ready_free();
  try {
    await replayPut({ name, bytes, saved: Date.now() });
    m2hleTools.add('replay: saved ' + name);
  } catch (e) {
    m2hleTools.add('replay: could not keep ' + name + ' in this browser (' + e + ')', 'warning');
  }
  if ($r('replay-download').checked) replayDownload(name, bytes);
  replayList();
}

function replayDownload(name, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/zip' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

/* ---- The list of kept replays ----------------------------------------------- */

function replayButton(text, fn) {
  const b = document.createElement('button');
  b.className = 'tool';
  b.type = 'button';
  b.textContent = text;
  b.addEventListener('click', fn);
  return b;
}

async function replayRow(rec) {
  const li = document.createElement('li');
  const j = await replayLabelOf(rec.bytes).catch(() => null);
  const head = document.createElement('div');
  head.textContent = j ? replayHeadline(j) : rec.name;
  const sub = document.createElement('div');
  sub.className = 'fine left';
  sub.textContent = j ? (j.stage || '?') + ' · ' + replayWhen(j) + ' · ' + replayClock(j.frames || 0)
                      : new Date(rec.saved).toLocaleString();
  const row = document.createElement('p');
  row.className = 'np-row';
  row.append(
    replayButton('Watch', () => replayOpen(rec.name, rec.bytes)),
    replayButton('Download', () => replayDownload(rec.name, rec.bytes)),
    replayButton('Delete', async () => {
      if (!confirm('Delete ' + rec.name + ' from this browser?')) return;
      await replayDel(rec.name);
      replayList();
    }));
  li.append(head, sub, row);
  return li;
}

async function replayList() {
  const ul = $r('replay-list');
  let all = [];
  try { all = await replayAll(); } catch (e) { /* private mode: no IndexedDB */ }
  all.sort((a, b) => b.saved - a.saved);
  const rows = await Promise.all(all.map(replayRow));
  ul.replaceChildren(...rows);
  $r('replay-none').hidden = all.length > 0;
}

/* ---- Wiring ----------------------------------------------------------------- */

function replayPick(e) {
  const f = e.target.files && e.target.files[0];
  e.target.value = '';
  if (!f) return;
  f.arrayBuffer().then((b) => replayOpen(f.name, new Uint8Array(b)));
}

function replayWire() {
  $r('replay-record').addEventListener('change', (e) => {
    replay.M._web_replay_set_record(e.target.checked ? 1 : 0);
    try { localStorage.setItem('m2hle.replayRecord', e.target.checked ? '1' : '0'); } catch (x) { /* private mode */ }
  });
  $r('replay-download').addEventListener('change', (e) => {
    try { localStorage.setItem('m2hle.replayDownload', e.target.checked ? '1' : '0'); } catch (x) { /* private mode */ }
  });
  $r('replay-playback').addEventListener('click', () => $r('replay-file').click());
  $r('replay-file').addEventListener('change', replayPick);
  $r('replay-play').addEventListener('click', replayPlay);
  $r('replay-notnow').addEventListener('click', () => { $r('replay-label').hidden = true; replayRefresh(); });
  $r('replay-keep').addEventListener('click', async () => {
    if (!replay.bytes) return;
    await replayPut({ name: replay.name, bytes: replay.bytes, saved: Date.now() });
    replayList();
  });
  $r('replay-hide').addEventListener('change', replayRefresh);
  $r('replay-stop').addEventListener('click', replayStop);
  $r('replay-restart').addEventListener('click', () => replay.M._web_replay_seek(0));
  $r('replay-fast').addEventListener('change', (e) => replay.M._web_replay_fast(e.target.checked ? 1 : 0));
  const seek = $r('replay-seek');
  seek.addEventListener('input', () => {
    replay.seeking = true;
    $r('replay-time').textContent = replayClock(Number(seek.value)) + ' / ' + replayClock(Number(seek.max));
  });
  seek.addEventListener('change', () => {
    replay.seeking = false;
    if (replay.M._web_replay_seek(Number(seek.value) >>> 0) !== 0) replaySay(replayError());
  });
}

function replayLoadSettings() {
  let rec = '0', dl = '0';
  try {
    rec = localStorage.getItem('m2hle.replayRecord') || '0';
    dl = localStorage.getItem('m2hle.replayDownload') || '0';
  } catch (e) { /* private mode */ }
  replay.M._web_replay_set_record(rec === '1' ? 1 : 0);
  $r('replay-download').checked = dl === '1';
}

const m2hleReplay = {
  onReady(module) {
    replay.M = module;
    if (!module._web_replay_status || !$r('pane-replay')) return;
    replayWire();
    replayLoadSettings();
    replayList();
    replay.timer = setInterval(() => {
      replayTakeReady();
      if (!$r('pane-replay').hidden || replayStatus().on) replayRefresh();
    }, REPLAY_POLL_MS);
    replayRefresh();
  },
  /* For a test script: open bytes and play them without the prompt. */
  open: (name, bytes) => replayOpen(name, bytes),
  play: () => replayPlay(),
  stop: () => replayStop(),
};
