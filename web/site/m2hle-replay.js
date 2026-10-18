/* Replays of games and online matches (core/replay.h, Pinboard #572, #617).
 *
 * With "Save a replay of every game and online match" on, each game played here and
 * each match this board plays on RPCN or GGPO ends as a .m2replay file (a zip: every
 * round in STF's own replay format, or with "Save the buttons" the board's state at
 * the start and both players' inputs; and replay.json, its label). The board leaves it in
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

/* Names and stages come from the board's label as they are; so do the board's
 * own errors, but for the three things Finish replay says. */
m2hleLang.add({
  en: {
    'replay.picked': '{name} ({slot}P)',
    'replay.vs': '{a}  vs  {b}',
    'replay.who': '{name} ({character})',
    'replay.nofight': 'No fight recorded',
    'replay.difficulty': 'Difficulty: {level}',
    'replay.saved.manual': 'Saved manually',
    'replay.saved.auto': 'Saved automatically',
    'replay.result.solo': 'Ended: {why}',
    'replay.result.win': '{winner} beat {loser}, {won}-{lost}',
    'replay.result.none': 'No result: {why}',
    'replay.result.early': 'it ended early',
    'replay.stage': 'Stage: {stage}',
    'replay.course': 'Course: {stage}',
    'replay.when': 'Played {when}, {length} long',
    'replay.length': '{when}, {length} long',
    'replay.by.local': 'played here; {romset}, {profile}',
    'replay.by.rpcn': 'recorded by {who}; {romset}, {profile}',
    'replay.by.ggpo': 'recorded by {who} over GGPO; {romset}, {profile}',
    'replay.busy': 'A replay is playing: Stop it first.',
    'replay.open.error': '{name}: {error}',
    'replay.stopped': 'Stopped: your own game is back, from power-on.',
    'replay.unplayable': 'Cannot play here: {why}',
    'replay.split': 'The replay went off its record: {why}',
    'replay.end': 'The end. Restart, drag back, or Stop to go back to your game.',
    'replay.seeking': 'Seeking...',
    'replay.finish.title': 'Finish the replay: save your game so far',
    'replay.finish.aria': 'Finish the replay',
    'replay.finish.saved': 'replay saved',
    'replay.finish.noround': 'not kept: no round has finished yet',
    'replay.finish.short': 'not kept: too short',
    'replay.watch': 'Watch',
    'replay.download': 'Download',
    'replay.delete': 'Delete',
    'replay.delete.ask': 'Delete {name} from this browser?',
    'replay.step.start': 'start a game (insert a coin, then press Start) or an online match',
    'replay.step.start.inputs': 'start a game (insert coins, then press Start)',
    'replay.step.kept': 'A game shorter than about 10 seconds is not kept, nor, unless you save the buttons from power-on, one with no round finished yet.',
    'replay.step.kept.inputs': 'A game shorter than about 10 seconds is not kept.',
    'replay.about': 'Replays are kept in this browser and listed below. A replay is each round in the game\'s own replay format, the one attract plays, a few KB a round; with the buttons from power-on it is the match\'s start and both players\' buttons, a few hundred KB, and plays the whole game. It plays only on the same game.',
    'replay.about.inputs': 'Replays are kept in this browser and listed below. {title} keeps no replay of its own, so a replay is the game\'s start and your buttons, about 1.5 MB, from the coin-up to game over. It plays only on the same game.',
  },
  ja: {
    'replay.picked': '{name}（{slot}P）',
    'replay.vs': '{a}  vs  {b}',
    'replay.who': '{name}（{character}）',
    'replay.nofight': '対戦の記録なし',
    'replay.difficulty': '難易度：{level}',
    'replay.saved.manual': '手動で保存',
    'replay.saved.auto': '自動で保存',
    'replay.result.solo': '終了：{why}',
    'replay.result.win': '{winner} が {loser} に勝利（{won}-{lost}）',
    'replay.result.none': '勝敗なし：{why}',
    'replay.result.early': '途中で終了',
    'replay.stage': 'ステージ：{stage}',
    'replay.course': 'コース：{stage}',
    'replay.when': '{when} にプレイ、長さ {length}',
    'replay.length': '{when}、長さ {length}',
    'replay.by.local': 'このブラウザでプレイ（{romset}、{profile}）',
    'replay.by.rpcn': '{who} が録画（{romset}、{profile}）',
    'replay.by.ggpo': '{who} が GGPO で録画（{romset}、{profile}）',
    'replay.busy': 'リプレイを再生中です。先に停止してください。',
    'replay.open.error': '{name}：{error}',
    'replay.stopped': '停止しました。ご自分のゲームに電源投入から戻ります。',
    'replay.unplayable': 'ここでは再生できません：{why}',
    'replay.split': 'リプレイが記録とずれました：{why}',
    'replay.end': '最後まで再生しました。最初から再生するか、タイムラインを戻すか、停止でゲームに戻ります。',
    'replay.seeking': 'シーク中…',
    'replay.finish.title': 'リプレイを終える：ここまでのゲームを保存',
    'replay.finish.aria': 'リプレイを終える',
    'replay.finish.saved': 'リプレイを保存しました',
    'replay.finish.noround': '保存しません：まだ1ラウンドも終わっていません',
    'replay.finish.short': '保存しません：短すぎます',
    'replay.watch': '見る',
    'replay.download': 'ダウンロード',
    'replay.delete': '削除',
    'replay.delete.ask': '{name} をこのブラウザから削除しますか？',
    'replay.step.start': 'ゲームを始める（コインを入れてスタートを押す）か、オンライン対戦を始めます',
    'replay.step.start.inputs': 'ゲームを始めます（コインを入れてスタートを押す）',
    'replay.step.kept': '約10秒より短いゲームは保存されません。電源投入からのボタンを保存しない場合は、1ラウンドも終わっていないゲームも保存されません。',
    'replay.step.kept.inputs': '約10秒より短いゲームは保存されません。',
    'replay.about': 'リプレイはこのブラウザに保存され、下に一覧で表示されます。リプレイはゲーム自身のリプレイ形式（デモ画面で再生されるもの）で各ラウンドを記録し、1ラウンド数KBです。電源投入からのボタンを保存する場合は、対戦の開始時点と両プレイヤーのボタン操作を記録し（数百KB）、ゲーム全体を再生します。再生できるのは同じゲームだけです。',
    'replay.about.inputs': 'リプレイはこのブラウザに保存され、下に一覧で表示されます。{title} にはゲーム自身のリプレイがないため、リプレイはゲームの開始時点とボタン操作を、コイン投入からゲームオーバーまで記録します（約1.5MB）。再生できるのは同じゲームだけです。',
  },
});
const replayT = m2hleLang.t;
/* Dates in the page's language; English keeps the browser's own. */
const REPLAY_LOCALE = m2hleLang.lang === m2hleLang.DEFAULT ? undefined : m2hleLang.lang;

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

/* A one-player game's replay (Sega Rally's race): its car and its course. */
const replaySolo = (j) => j.players === 1;

/* A game on this board: the characters the players picked, not the CPU's
 * (Pinboard #875). An online match: who played whom, with what. */
function replayHeadline(j) {
  if (j.netcode === 'local') {
    const picked = [1, 2].filter((s) => j['p' + s + '_character'] && !j['p' + s + '_cpu'])
      .map((s) => replayT('replay.picked', { name: j['p' + s + '_character'], slot: s }));
    if (picked.length === 2) return replayT('replay.vs', { a: picked[0], b: picked[1] });
    return picked.length ? picked[0] : replayT('replay.nofight');
  }
  const who = (p, c) => (c ? replayT('replay.who', { name: p || '?', character: c }) : p || '?');
  if (replaySolo(j)) return who(j.p1, j.p1_character);
  return replayT('replay.vs', { a: who(j.p1, j.p1_character), b: who(j.p2, j.p2_character) });
}

/* A game's difficulty and whether it was saved by itself or by Finish replay. */
function replayHow(j) {
  const how = [];
  if (j.difficulty) how.push(replayT('replay.difficulty', { level: j.difficulty }));
  if (j.stopped) how.push(replayT(j.stopped === 'manual' ? 'replay.saved.manual' : 'replay.saved.auto'));
  return how.join(' · ');
}

function replayResult(j) {
  if (replaySolo(j)) return replayT('replay.result.solo', { why: j.ended || '?' });
  if (j.winner === 0 || j.winner === 1) {
    const w = j.winner === 0 ? j.p1_rounds : j.p2_rounds, l = j.winner === 0 ? j.p2_rounds : j.p1_rounds;
    return replayT('replay.result.win', { winner: j.winner_name, loser: j.loser_name, won: w | 0, lost: l | 0 });
  }
  return replayT('replay.result.none', { why: j.ended || replayT('replay.result.early') });
}

const replayWhen = (j) => (j.timestamp ? new Date(j.timestamp * 1000).toLocaleString(REPLAY_LOCALE) : '?');
const replayLength = (j) => replayT('replay.length', { when: replayWhen(j), length: replayClock(j.frames || 0) });

function replaySay(text) { $r('replay-status').textContent = text || ''; }

/* ---- The board -------------------------------------------------------------- */

const replayStatus = () => JSON.parse(replay.M.UTF8ToString(replay.M._web_replay_status()));
const replayError = () => replay.M.UTF8ToString(replay.M._web_replay_error());

/* Hand a file to the board and show its label. */
function replayOpen(name, bytes) {
  const M = replay.M;
  if (!M || !M._web_replay_open) return;
  const st = replayStatus();
  if (st.on) { replaySay(replayT('replay.busy')); return; }
  const ptr = M._malloc(bytes.length || 1);
  M.HEAPU8.set(bytes, ptr);                      /* the board frees it */
  if (M._web_replay_open(ptr, bytes.length) !== 0) {
    replaySay(replayT('replay.open.error', { name, error: replayError() }));
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
  $r('replay-stage').textContent = j.stage ? replayT(replaySolo(j) ? 'replay.course' : 'replay.stage', { stage: j.stage }) : '';
  $r('replay-how').textContent = replayHow(j);
  $r('replay-when').textContent = replayT('replay.when', { when: replayWhen(j), length: replayClock(j.frames || 0) });
  const by = j.netcode === 'local' ? 'local' : j.netcode === 'ggpo' ? 'ggpo' : 'rpcn';
  $r('replay-by').textContent = replayT('replay.by.' + by, { who: j.recorded_by || '?', romset: j.romset, profile: j.profile });
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
  replaySay(replayT('replay.stopped'));
  replayRefresh();
}

/* What the panel shows follows the board: the prompt, the controls, or why not. */
/* The REPLAY section: the switch, and whether a match is being recorded. */
function replayRefreshRecord(st) {
  $r('replay-record').checked = !!st.record;
  $r('replay-recording').hidden = !st.recording;
  $r('replay-rec-error').textContent = st.rec_error || '';
}

/* The bar's Finish replay, beside Pause (Pinboard #861): only while the
 * player's own game is being recorded. That records until the game goes back to
 * attract, which free play and continues can put off for ever. */
function replayFinishRender(st) {
  const btn = $r('btn-replay-finish');
  if (btn && replay.M._web_replay_finish) btn.hidden = !st.local;
}

/* What the board says (core/replay.h, replay_rec_finish_local), by key. */
const REPLAY_FINISH_SAID = {
  'replay saved': 'replay.finish.saved',
  'not kept: no round has finished yet': 'replay.finish.noround',
  'not kept: too short': 'replay.finish.short',
};

function replayFinish() {
  const said = replay.M.UTF8ToString(replay.M._web_replay_finish());
  if (!said) return;
  m2hleTools.add('replay: ' + said);
  const key = REPLAY_FINISH_SAID[said];
  replaySay(key ? replayT(key) : said);
  replayTakeReady();
  replayRefresh();
}

function replayRefresh() {
  if (!replay.M || !replay.M._web_replay_status) return;
  const st = replayStatus();
  replayRefreshRecord(st);
  replayFinishRender(st);
  $r('replay-result').hidden = $r('replay-hide').checked;
  const open = st.loaded && !$r('replay-label').hidden;
  $r('replay-ask').hidden = !open || st.on || !!st.unplayable;
  $r('replay-unplayable').hidden = !open || st.on || !st.unplayable;
  $r('replay-unplayable').textContent = st.unplayable ? replayT('replay.unplayable', { why: st.unplayable }) : '';
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
  $r('replay-end').textContent = st.split ? replayT('replay.split', { why: st.why })
    : st.ended ? replayT('replay.end')
    : st.seeking ? replayT('replay.seeking') : '';
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
  sub.textContent = j && j.netcode === 'local' ? replayHow(j) || replayWhen(j)
    : j ? (j.stage || '?') + ' · ' + replayWhen(j) + ' · ' + replayClock(j.frames || 0)
    : new Date(rec.saved).toLocaleString(REPLAY_LOCALE);
  if (j) li.title = replayLength(j);
  const row = document.createElement('p');
  row.className = 'np-row';
  row.append(
    replayButton(replayT('replay.watch'), () => replayOpen(rec.name, rec.bytes)),
    replayButton(replayT('replay.download'), () => replayDownload(rec.name, rec.bytes)),
    replayButton(replayT('replay.delete'), async () => {
      if (!confirm(replayT('replay.delete.ask', { name: rec.name }))) return;
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
  const finish = $r('btn-replay-finish');
  finish.title = replayT('replay.finish.title');
  finish.setAttribute('aria-label', replayT('replay.finish.aria'));
  finish.addEventListener('click', () => {
    replayFinish();
    if (typeof leaveBar === 'function') leaveBar();   /* m2hle-page.js: the canvas gets the keys back */
  });
  $r('replay-record').addEventListener('change', (e) => {
    replay.M._web_replay_set_record(e.target.checked ? 1 : 0);
    try { localStorage.setItem('m2hle.replayRecord', e.target.checked ? '1' : '0'); } catch (x) { /* private mode */ }
  });
  $r('replay-download').addEventListener('change', (e) => {
    try { localStorage.setItem('m2hle.replayDownload', e.target.checked ? '1' : '0'); } catch (x) { /* private mode */ }
  });
  $r('replay-as-inputs').addEventListener('change', (e) => {
    replay.M._web_replay_set_format(e.target.checked ? 1 : 0);
    try { localStorage.setItem('m2hle.replayFormat', e.target.checked ? 'inputs' : 'native'); } catch (x) { /* private mode */ }
  });
  $r('replay-view-inputs').addEventListener('change', (e) => {
    replay.M._web_replay_set_view(e.target.checked ? 1 : 0);
    try { localStorage.setItem('m2hle.replayView', e.target.checked ? 'inputs' : 'native'); } catch (x) { /* private mode */ }
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
  let rec = '0', dl = '0', fmt = 'native', view = 'native';
  try {
    rec = localStorage.getItem('m2hle.replayRecord') || '0';
    dl = localStorage.getItem('m2hle.replayDownload') || '0';
    fmt = localStorage.getItem('m2hle.replayFormat') || 'native';
    view = localStorage.getItem('m2hle.replayView') || 'native';
  } catch (e) { /* private mode */ }
  replay.M._web_replay_set_record(rec === '1' ? 1 : 0);
  $r('replay-download').checked = dl === '1';
  $r('replay-as-inputs').checked = fmt === 'inputs';
  $r('replay-view-inputs').checked = view === 'inputs';
  if (replay.M._web_replay_set_format) {
    replay.M._web_replay_set_format(fmt === 'inputs' ? 1 : 0);
    replay.M._web_replay_set_view(view === 'inputs' ? 1 : 0);
  }
}

/* A game with no replay of its own (m2hleGame.ownReplay false) saves its
 * buttons whatever the format: the two format boxes have nothing to pick. */
function replayGameText() {
  const inputs = m2hleGame.ownReplay === false, v = inputs ? '.inputs' : '';
  $r('replay-as-inputs').closest('p').hidden = inputs;
  $r('replay-view-inputs').closest('p').hidden = inputs;
  $r('replay-step-start').textContent = replayT('replay.step.start' + v);
  $r('replay-step-kept').textContent = replayT('replay.step.kept' + v);
  $r('replay-about').textContent = replayT('replay.about' + v, { title: m2hleGame.title });
}

const m2hleReplay = {
  onReady(module) {
    replay.M = module;
    if (!module._web_replay_status || !$r('pane-replay')) return;
    replayGameText();
    replayWire();
    replayLoadSettings();
    replayList();
    replay.timer = setInterval(() => {
      replayTakeReady();
      const st = replayStatus();
      if (!$r('pane-replay').hidden || st.on) replayRefresh();
      else replayFinishRender(st);
    }, REPLAY_POLL_MS);
    replayRefresh();
  },
  /* For a test script: open bytes and play them without the prompt. */
  open: (name, bytes) => replayOpen(name, bytes),
  play: () => replayPlay(),
  stop: () => replayStop(),
};
