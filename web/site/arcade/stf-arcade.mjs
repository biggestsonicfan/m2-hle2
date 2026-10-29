// The arcade, on the page that runs the game (m2-hle2's web build at arcade.sonicthefighte.rs,
// ?game=sfight, ?game=srallyc, ...).
//
//   <meta name="stf-arcade-api" content="https://scores.sonicthefighte.rs">
//   <script type="module" src="arcade/stf-arcade.mjs?game=srallyc&profile=srallyc"></script>
//
// ?game= names the game (games.mjs): sfight, the default, or any other listed there. Each
// has its own shared board, its own cookies and its own words; only Sonic the Fighters'
// mention the Fly, which plays only Sonic. ?profile= is the m2-hle2 profile that runs: a
// game run as several (games.mjs `sets`) keeps the board under that profile's own key, and
// a profile that does not share the board, or a game the arcade does not know, plays alone
// (the module then only answers the page: ready, keepSave() true).
//
// The page ships its own copy of this file, ranking.mjs and stf-arcade.css (m2-hle2's
// web/site/arcade/: its build refuses scripts from other sites); only the API calls go to
// the arcade. Without the meta tag the API is wherever this module came from.
//
// Asks once, plainly, whether the player wants to share, and keeps the answer in a cookie.
// Two separate choices, each can be turned off at any time from the "Arcade & your data"
// button:
//   arcade  the game boots on the shared board (the Fly's backup RAM: its ranking, its
//           settings), and new ranking entries (initials + clear time) go back to it.
//   usage   a few anonymous events: page opened, game started (and how), time played.
// Saying no to both keeps the game as it was: its own board, kept in this browser. Sega
// Rally asks that too (its third choice, keep): until the player says yes to something,
// its save is not kept from one visit to the next.
//
// The page calls, if this module is there:
//   await window.stfArcade.ready    before web_rom_load (the board must be in localStorage)
//   window.stfArcade.keepSave()     whether the board goes in localStorage (web_backup_keep)
//   window.stfArcade.onGame(mode)   once the game runs: 'solo' 'netplay' 'ggpo' 'follow'

import { hexToBytes, newEntries } from './ranking.mjs';
import { gameFor, nvramKeyFor } from './games.mjs';

const ASKED = new URL(import.meta.url).searchParams;
const GAME = gameFor(ASKED.get('game'));
const API = (document.querySelector('meta[name=stf-arcade-api]') || {}).content ||
  new URL('.', import.meta.url).origin;
const NVRAM_KEY = GAME && nvramKeyFor(GAME, ASKED.get('profile'));
const LOCAL_KEY = NVRAM_KEY + '.local';      // the player's own board, kept while they play ours
const YEAR = 365 * 24 * 3600;

/* What differs on the page from one game to the next. Sonic the Fighters' names are the
 * ones its page has always used, so nobody's answer or id is lost. */
const PAGE = {
  sfight: {
    cookie: 'stf', sentKey: 'stfArcade.sent', asksKeep: false, privacy: 'privacy.html',
  },
  srallyc: {
    cookie: 'srally', sentKey: 'srallyArcade.sent', asksKeep: true, privacy: 'srally-privacy.html',
  },
}[GAME && GAME.id] || (GAME && {
  // Every other game: its own cookies (named for its id, so no other game's answer is
  // reused), its save kept in this browser as m2-hle2 always kept it, words without the Fly.
  cookie: GAME.id, sentKey: GAME.id + 'Arcade.sent', asksKeep: false,
  privacy: `game-privacy.html?game=${GAME.id}`, words: GAME.score ? 'score' : 'time',
});
const SENT_KEY = PAGE && PAGE.sentKey;       // entries the shared board already has

/* The words, by language (Pinboard #944), as HTML: the game page's m2hleLang picks one
 * (?lang=); without it they are English. A game's own are arcade.<game id>.*. */
const WORDS = {
  en: {
    'arcade.sfight.what': `<p><b>Arcade: shared high scores.</b> Your game starts on the same board as everyone
else's, the one the Fly plays on the stream, and fights for the same ranking. When you set
a time, its <b>initials and clear time</b> are sent to the arcade and shown to everyone.</p>`,
    'arcade.sfight.share': 'Arcade: share my high scores, play on the shared board',
    'arcade.sfight.only': 'High scores only',
    'arcade.srallyc.what': `<p><b>Arcade: shared best times.</b> Your game starts on the same board as everyone
else's who plays Sega Rally here, and races for the same best times on every course. When
you set one, its <b>initials, time and car</b> are sent to the arcade and shown to everyone.
Your settings and best times are kept in this browser too.</p>
<p><b>Keep my save here.</b> Or keep them in this browser only, and send nothing. Say no to
everything and every visit starts on a fresh board.</p>`,
    'arcade.srallyc.share': 'Arcade: share my best times, race on the shared board',
    'arcade.srallyc.only': 'Best times only',
    'arcade.score.what': `<p><b>Arcade: shared high scores.</b> Your game starts on the same board as everyone
else's who plays {title} here, and goes for the same high scores. When you set one, its
<b>initials and score</b> are sent to the arcade and shown to everyone.</p>`,
    'arcade.score.share': 'Arcade: share my high scores, play on the shared board',
    'arcade.score.only': 'High scores only',
    'arcade.time.what': `<p><b>Arcade: shared best times.</b> Your game starts on the same board as everyone
else's who plays {title} here, and goes for the same best times. When you set one, its
<b>initials and time</b> are sent to the arcade and shown to everyone.</p>`,
    'arcade.time.share': 'Arcade: share my best times, play on the shared board',
    'arcade.time.only': 'Best times only',
    'arcade.stats': `<b>Usage stats.</b> Anonymous counts that help us keep the site working: that the page
opened, that a game started (alone, netplay or watching), and how long it ran.
Nothing else: no address, no browser details, no tracking across sites.`,
    'arcade.ids': `Sharing either one gives this browser a random id (cookie <code>{player}</code>) so you
can see or delete what was sent. Your choice is kept in the cookie <code>{consent}</code>.
<a href="{privacy}" target="_blank" rel="noopener">The details</a>.`,
    'arcade.later': 'Your choice applies the next time the game starts. Reload the page to switch boards now.',
    'arcade.join': 'Join the arcade?',
    'arcade.both': 'Join, share both',
    'arcade.keepOnly': 'Keep my save here, send nothing',
    'arcade.noKeep': 'No thanks, keep nothing',
    'arcade.noAlone': 'No thanks, play alone',
    'arcade.keepRow': 'Keep my save in this browser (also without the arcade; off deletes it)',
    'arcade.usageRow': 'Usage stats: anonymous counts of visits, games and play time',
    'arcade.panel': 'Arcade &amp; your data',
    'arcade.panelAria': 'Arcade and your data',
    'arcade.save': 'Save',
    'arcade.show': 'What have you got on me?',
    'arcade.forget': 'Delete what I sent',
    'arcade.close': 'Close',
    'arcade.nothing': 'Nothing: this browser has not sent anything.',
  },
  ja: {
    'arcade.sfight.what': `<p><b>アーケード：ハイスコアの共有。</b>みんなと同じ基板、配信で Fly がプレイしている基板でゲームが始まり、同じランキングを競います。記録を出すと、その<b>イニシャルとクリアタイム</b>がアーケードに送られ、全員に表示されます。</p>`,
    'arcade.sfight.share': 'アーケード：ハイスコアを共有し、共有の基板でプレイする',
    'arcade.sfight.only': 'ハイスコアのみ',
    'arcade.srallyc.what': `<p><b>アーケード：ベストタイムの共有。</b>ここでセガラリーをプレイするみんなと同じ基板でゲームが始まり、すべてのコースで同じベストタイムを競います。記録を出すと、その<b>イニシャル、タイム、車</b>がアーケードに送られ、全員に表示されます。設定とベストタイムはこのブラウザにも保存されます。</p>
<p><b>セーブをここに保存。</b>または、このブラウザだけに保存して何も送らないこともできます。すべて断ると、毎回まっさらな基板から始まります。</p>`,
    'arcade.srallyc.share': 'アーケード：ベストタイムを共有し、共有の基板でレースする',
    'arcade.srallyc.only': 'ベストタイムのみ',
    'arcade.score.what': `<p><b>アーケード：ハイスコアの共有。</b>ここで{title}をプレイするみんなと同じ基板でゲームが始まり、同じハイスコアを競います。記録を出すと、その<b>イニシャルとスコア</b>がアーケードに送られ、全員に表示されます。</p>`,
    'arcade.score.share': 'アーケード：ハイスコアを共有し、共有の基板でプレイする',
    'arcade.score.only': 'ハイスコアのみ',
    'arcade.time.what': `<p><b>アーケード：ベストタイムの共有。</b>ここで{title}をプレイするみんなと同じ基板でゲームが始まり、同じベストタイムを競います。記録を出すと、その<b>イニシャルとタイム</b>がアーケードに送られ、全員に表示されます。</p>`,
    'arcade.time.share': 'アーケード：ベストタイムを共有し、共有の基板でプレイする',
    'arcade.time.only': 'ベストタイムのみ',
    'arcade.stats': `<b>利用統計。</b>サイトの運営に役立つ匿名の集計です。ページを開いたこと、ゲームを始めたこと（ひとりで、オンライン対戦で、観戦で）、そのプレイ時間だけを送ります。アドレスやブラウザの詳細は送らず、サイトをまたいだ追跡もしません。`,
    'arcade.ids': `どちらかを共有すると、送った内容を確認・削除できるよう、このブラウザにランダムな ID（Cookie <code>{player}</code>）が付きます。選択は Cookie <code>{consent}</code> に保存されます。<a href="{privacy}" target="_blank" rel="noopener">詳しくはこちら（英語）</a>。`,
    'arcade.later': '選択は次にゲームを始めたときから有効です。今すぐ基板を切り替えるには、ページを再読み込みしてください。',
    'arcade.join': 'アーケードに参加しますか？',
    'arcade.both': '参加して、両方とも共有',
    'arcade.keepOnly': 'セーブをここに保存し、何も送らない',
    'arcade.noKeep': '参加しない、何も保存しない',
    'arcade.noAlone': '参加しない、ひとりで遊ぶ',
    'arcade.keepRow': 'このブラウザにセーブを保存（アーケードなしでも。オフにすると削除します）',
    'arcade.usageRow': '利用統計：訪問、ゲーム、プレイ時間の匿名の集計',
    'arcade.panel': 'アーケードとデータ',
    'arcade.panelAria': 'アーケードとデータ',
    'arcade.save': '保存',
    'arcade.show': '送ったデータを見る',
    'arcade.forget': '送ったデータを削除',
    'arcade.close': '閉じる',
    'arcade.nothing': 'ありません。このブラウザはまだ何も送っていません。',
  },
};
const LANG = typeof m2hleLang === 'object' ? m2hleLang : null;   // a page script's global, not window's
if (LANG) LANG.add(WORDS);
const tr = (key, vars) => (LANG ? LANG.t(key, vars) :
  WORDS.en[key].replace(/\{(\w+)\}/g, (m, k) => (vars && k in vars ? vars[k] : m)));
/* The game's title in the page's language, when the page has its list (m2hle-games.js). */
function pageTitle() {
  try { return m2hleGames.text(GAME.id).title; } catch { return null; }   // no list, or not in it
}
const TITLE = GAME && (pageTitle() || GAME.title);
const own = (what) => tr(`arcade.${PAGE.words || GAME.id}.${what}`, { title: TITLE });

/* ---- The choice, in cookies ------------------------------------------------- */

function cookie(name) {
  const m = document.cookie.match(new RegExp('(?:^|; )' + name + '=([^;]*)'));
  return m ? decodeURIComponent(m[1]) : null;
}

function setCookie(name, value, maxAge = YEAR) {
  document.cookie = `${name}=${encodeURIComponent(value)}; Max-Age=${maxAge}; Path=/; SameSite=Lax; Secure`;
}

const CONSENT = PAGE && PAGE.cookie + '_consent', PLAYER = PAGE && PAGE.cookie + '_player';
const CHOICES = ['arcade', 'usage', 'keep'];

/* { arcade, usage, keep } once they chose, null before. */
export function readConsent() {
  const v = cookie(CONSENT);
  if (v === null) return null;
  const on = v.split('.');
  return { arcade: on.includes('arcade'), usage: on.includes('usage'), keep: on.includes('keep') };
}

/* Whether the game's board is kept in this browser: always for Sonic the Fighters, as it
 * always was; for a game that asks, once the player said yes to the arcade or to keeping. */
export function keepSave() {
  if (!PAGE.asksKeep) return true;
  const c = readConsent();
  return !!(c && (c.arcade || c.keep));
}

function newPlayerId() {
  const b = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(b, (x) => (x | 256).toString(16).slice(1)).join('');
}

function player() { return cookie(PLAYER); }

function saveConsent(c) {
  const on = CHOICES.filter((k) => c[k]);
  setCookie(CONSENT, on.length ? on.join('.') : 'none');
  const sends = c.arcade || c.usage;           // keeping a save here sends nothing: no id
  if (sends && !player()) setCookie(PLAYER, newPlayerId());
  if (!sends) setCookie(PLAYER, '', 0);
}

/* ---- Talking to the arcade ------------------------------------------------- */

/* text/plain keeps these "simple" requests: no preflight, and sendBeacon can carry them. */
function post(path, body, { beacon = false } = {}) {
  const text = JSON.stringify(body);
  if (beacon && navigator.sendBeacon) {
    navigator.sendBeacon(API + path, new Blob([text], { type: 'text/plain' }));
    return Promise.resolve(null);
  }
  return fetch(API + path, { method: 'POST', body: text, headers: { 'Content-Type': 'text/plain' },
    keepalive: true }).then((r) => (r.ok && r.status !== 204 ? r.json() : null));
}

function usage(event, data = {}, opts) {
  const c = readConsent();
  if (c && c.usage && player()) {
    post('/api/usage', { player: player(), event, data: { ...data, game: GAME.id } }, opts).catch(() => {});
  }
}

/* ---- The board ----------------------------------------------------------- */

function localBoard() {
  const bytes = hexToBytes(localStorage.getItem(NVRAM_KEY) || '');
  return bytes && bytes.length === GAME.backupSize ? bytes : null;
}

/* Every entry on a board as { table, hex }. The sent list names one as its hex in the first
 * table and table:hex in the others, so Sonic the Fighters' list reads as it always did. */
function boardEntries(board) {
  return GAME.readTables(board).flatMap((t, table) => t.map((e) => ({ table, hex: e.hex })));
}

const sentName = (e) => (e.table ? `${e.table}:${e.hex}` : e.hex);

function sent() { try { return JSON.parse(localStorage.getItem(SENT_KEY)) || []; } catch { return []; } }

/* Puts the shared board where the emulator will read it, keeping the player's own aside. */
async function joinBoard() {
  const r = await fetch(`${API}/api/board?game=${GAME.id}`,
    { signal: AbortSignal.timeout(5000) });   // never hold the game up long
  if (!r.ok) throw new Error('board: ' + r.status);
  const { set, hex } = await r.json();
  if (set !== GAME.id) throw new Error('board: ' + set);   // an arcade that knows no ?game=
  const mine = localStorage.getItem(NVRAM_KEY);
  if (mine !== null && localStorage.getItem(LOCAL_KEY) === null) localStorage.setItem(LOCAL_KEY, mine);
  localStorage.setItem(NVRAM_KEY, hex);
  localStorage.setItem(SENT_KEY, JSON.stringify(boardEntries(hexToBytes(hex)).map(sentName)));
}

/* Gives the player their own board back (from the next time the game starts). */
function leaveBoard() {
  const mine = localStorage.getItem(LOCAL_KEY);
  if (mine !== null && keepSave()) localStorage.setItem(NVRAM_KEY, mine);
  else localStorage.removeItem(NVRAM_KEY);
  localStorage.removeItem(LOCAL_KEY);
  localStorage.removeItem(SENT_KEY);
}

function joined() { return localStorage.getItem(SENT_KEY) !== null; }

/* Sends what the player put on the ranking since the board loaded. */
function syncScores(opts) {
  const c = readConsent();
  const board = c && c.arcade && joined() && localBoard();
  if (!board || !player()) return;
  // Compared by sent name (the table and the entry), sent as the entry itself.
  const mine = boardEntries(board).map((e) => ({ table: e.table, entry: e.hex, hex: sentName(e) }));
  const fresh = newEntries(sent().map((hex) => ({ hex })), mine);
  if (!fresh.length) return;
  localStorage.setItem(SENT_KEY, JSON.stringify(sent().concat(fresh.map((e) => e.hex))));
  // Sonic the Fighters' as it always sent them (hex, no game): an older arcade takes them too.
  const entries = GAME.id === 'sfight' ? fresh.map((e) => e.entry)
    : fresh.map(({ table, entry }) => ({ table, hex: entry }));
  post('/api/scores', { game: GAME.id, player: player(), entries }, opts).catch(() => {});
}

/* ---- What the page sees -------------------------------------------------- */

let started = 0;

async function prepare() {
  const c = readConsent();
  if (c && c.arcade) {
    try { await joinBoard(); } catch (e) { console.warn('arcade: playing on your own board,', e.message); }
  } else if (joined()) leaveBoard();
  if (!keepSave()) forgetSave();
}

/* Not kept: the game's board goes from this browser (the running game stops writing it). */
function forgetSave() {
  localStorage.removeItem(NVRAM_KEY);
  localStorage.removeItem(LOCAL_KEY);
}

/* A running game follows a change of keep at once: on, it stores what it holds now. */
function applyKeep() {
  if (!started || !window.Module || !Module._web_backup_keep) return;
  Module._web_backup_keep(keepSave() ? 1 : 0);
}

function onGame(mode = 'solo') {
  started = Date.now();
  usage('game_start', { mode });
  setInterval(syncScores, 60000);        // the emulator keeps its backup RAM once a minute too
}

addEventListener('pagehide', () => {
  if (!started) return;
  if (window.Module && Module._web_backup_flush) Module._web_backup_flush();
  syncScores({ beacon: true });
  usage('play_time', { seconds: (Date.now() - started) / 1000 }, { beacon: true });
});

/* ---- The banner and the panel -------------------------------------------- */

const WHAT = PAGE && `${own('what')}
<p>${tr('arcade.stats')}</p>
<p>${tr('arcade.ids', { player: PLAYER, consent: CONSENT, privacy: `${API}/${PAGE.privacy}` })}</p>`;

function el(html) {
  const t = document.createElement('template');
  t.innerHTML = html.trim();
  return t.content.firstChild;
}

function choose(c, box) {
  const before = readConsent();
  const kept = keepSave();
  saveConsent(c);
  box.remove();
  if (!keepSave() && kept) forgetSave();
  applyKeep();
  if (!!(before && before.arcade) === c.arcade) return;
  if (started) note(tr('arcade.later'));
  else ready = ready.then(prepare);       // the game has not loaded yet: switch boards now
}

function note(text) {
  const n = el(`<div class="stfa-note" role="status">${text}</div>`);
  document.body.append(n);
  setTimeout(() => n.remove(), 8000);
}

function banner() {
  const box = el(`<div class="stfa-banner" role="dialog" aria-label="${tr('arcade.join')}">
    <h2>${tr('arcade.join')}</h2>${WHAT}
    <div class="stfa-buttons">
      <button data-a="all">${tr('arcade.both')}</button>
      <button data-a="arcade">${own('only')}</button>
      ${PAGE.asksKeep ? `<button data-a="keep">${tr('arcade.keepOnly')}</button>` : ''}
      <button data-a="none">${tr(PAGE.asksKeep ? 'arcade.noKeep' : 'arcade.noAlone')}</button>
    </div></div>`);
  box.addEventListener('click', (e) => {
    const a = e.target.dataset && e.target.dataset.a;
    if (a) choose({ arcade: a === 'all' || a === 'arcade', usage: a === 'all', keep: a === 'keep' }, box);
  });
  document.body.append(box);
}

function panelRows(c) {
  const keep = PAGE.asksKeep ? `<label><input type="checkbox" name="keep" ${c.keep ? 'checked' : ''}>
      ${tr('arcade.keepRow')}</label>` : '';
  return `<label><input type="checkbox" name="arcade" ${c.arcade ? 'checked' : ''}>
      ${own('share')}</label>
    <label><input type="checkbox" name="usage" ${c.usage ? 'checked' : ''}>
      ${tr('arcade.usageRow')}</label>${keep}`;
}

function panel() {
  const c = readConsent() || { arcade: false, usage: false, keep: false };
  const box = el(`<div class="stfa-banner" role="dialog" aria-label="${tr('arcade.panelAria')}">
    <h2>${tr('arcade.panel')}</h2>${WHAT}${panelRows(c)}
    <pre class="stfa-mine" hidden></pre>
    <div class="stfa-buttons">
      <button data-a="save">${tr('arcade.save')}</button>
      <button data-a="show">${tr('arcade.show')}</button>
      <button data-a="forget">${tr('arcade.forget')}</button>
      <button data-a="close">${tr('arcade.close')}</button>
    </div></div>`);
  box.addEventListener('click', (e) => panelAction(e.target.dataset && e.target.dataset.a, box));
  document.body.append(box);
}

async function panelAction(a, box) {
  const pre = box.querySelector('.stfa-mine');
  if (a === 'save') {
    const on = (name) => !!(box.querySelector(`[name=${name}]`) || {}).checked;
    choose({ arcade: on('arcade'), usage: on('usage'), keep: on('keep') }, box);
  } else if (a === 'close') box.remove();
  else if (a === 'show' || a === 'forget') {
    pre.hidden = false;
    if (!player()) { pre.textContent = tr('arcade.nothing'); return; }
    const out = await post(a === 'show' ? '/api/me' : '/api/forget', { player: player() }).catch((e) => e.message);
    pre.textContent = JSON.stringify(out, null, 1);
    if (a === 'forget') { setCookie(PLAYER, '', 0); saveConsent(readConsent() || {}); }
  }
}

function start() {
  const css = el(`<link rel="stylesheet" href="${new URL('stf-arcade.css', import.meta.url)}">`);
  document.head.append(css);
  const button = el(`<button class="stfa-open" title="${tr('arcade.panel')}">${tr('arcade.panel')}</button>`);
  button.addEventListener('click', panel);
  document.body.append(button);
  if (!readConsent()) banner();
}

let ready;
if (NVRAM_KEY) {
  ready = prepare().then(() => usage('visit'));
  window.stfArcade = { get ready() { return ready; }, onGame, readConsent, keepSave, game: GAME.id };
  if (document.body) start(); else addEventListener('DOMContentLoaded', start);
} else {
  // No board for this game or profile: the game keeps its own save, and nothing is asked.
  window.stfArcade = { ready: Promise.resolve(), onGame() {}, readConsent: () => null,
    keepSave: () => true, game: null };
}
