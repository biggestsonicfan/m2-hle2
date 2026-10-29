/*
 * The site's languages (Pinboard #935). Loaded first by both pages, before
 * m2hle-games.js.
 *
 *   arcade.sonicthefighte.rs/?lang=ja&game=vonj
 *
 * ?lang= is a key of LANGS; one the site does not have is English. Without it
 * (Pinboard #1099) the page takes the language this browser last asked for
 * with ?lang= (localStorage m2hle2.lang), and failing that guesses: Japanese
 * when the visitor seems to be in Japan (the clock on Japan time, or Japanese
 * first among the browser's languages), English otherwise. A static site sees
 * no address, so the clock and the browser's languages are what it has. The
 * switch on the home page always names its language, so a choice sticks. The language is the page's words only: which game and
 * which profile run is ?game= and ?profile= (m2hle-games.js), so a Japanese
 * page can play an English set and the other way round.
 *
 * A language is a table of strings by key. English is the template: every key
 * is in it, and a key another language leaves out shows its English, so a new
 * language can start with the few lines that matter and grow. To add one, copy
 * `en`, translate, and add it to LANGS.
 *
 * The page names what to translate with attributes:
 *   data-i18n="key"       the element's text
 *   data-i18n-html="key"  its HTML (only for the tables' own markup, <code>)
 *   data-i18n-title="key", data-i18n-aria="key"  its title / aria-label
 *   data-i18n-placeholder="key", data-i18n-alt="key"  an input's placeholder, an image's alt
 * A game's own words (title, blurb, profile names, ROM hint, control labels)
 * are in its GAMES entry, with each language's under `i18n` (m2hleGames.text).
 *
 * The game page's menu and its panels (Pinboard #944) bring their own words:
 * m2hle-lang-menu.js the page's markup, and each script that writes text (the
 * lag check, the lobby, replays, ...) its own keys, with add() at its top.
 * index.html translates its markup itself, just before those scripts run;
 * another page is translated at DOMContentLoaded.
 */
'use strict';

const m2hleLang = (() => {
  const en = {
    'home.title': 'Model 2 Arcade',
    'home.lead': 'Sega Model 2 games in your browser, run by m2-hle2. Bring your own copy of the game: it stays on your computer.',
    'home.noscript': 'This page needs JavaScript to run the games.',
    'home.soon': '{about}: not in the emulator yet',
    'home.desc': 'Play Sega Model 2 arcade games in your browser, with your own copy of the game.',
    'site.source': 'Source on GitHub',
    'site.games': 'All games',
    'site.language': 'Language',
    'game.desc': 'Play {title} in your browser, with your own copy of the game.',
    'step.loading': 'Starting up…',
    'step.rom': 'Add your copy of the game to begin.',
    'step.choose': 'Choose your game file',
    'step.drop': 'or drop it here',
    'step.private': 'Your file stays on this computer. It is read by this page and is never uploaded.',
    'step.busy': 'Reading your game file…',
    'step.fatal': 'This browser could not start the game.',
    'step.reload': 'Reload',
    'hint.rom': ':Choose file',
    'rom.missing.parent': 'This zip is missing {file}, which the game needs. If it is a split set, the rest is in its parent set, {parent}: add {parent} too.',
    'rom.missing.more': 'These zips are missing {file}, which the game needs. Choose the game\'s zip and {parent} together, or a merged set.',
  };

  const ja = {
    'home.title': 'Model 2 アーケード',
    'home.lead': 'セガ Model 2 のゲームをブラウザで。エミュレーターは m2-hle2 です。ゲームはご自分のものをお使いください。ファイルはお使いのコンピューターから出ません。',
    'home.noscript': 'ゲームを動かすには JavaScript が必要です。',
    'home.soon': '{about}：エミュレーター未対応',
    'home.desc': 'セガ Model 2 のアーケードゲームを、ご自分のゲームファイルでブラウザから遊べます。',
    'site.source': 'GitHub のソース',
    'site.games': 'ゲーム一覧',
    'site.language': '言語',
    'game.desc': '{title} をご自分のゲームファイルでブラウザから遊べます。',
    'step.loading': '起動中…',
    'step.rom': 'はじめにゲームファイルを選んでください。',
    'step.choose': 'ゲームファイルを選ぶ',
    'step.drop': 'またはここにドロップ',
    'step.private': 'ファイルはこのページが読み込むだけで、アップロードはされません。',
    'step.busy': 'ゲームファイルを読み込み中…',
    'step.fatal': 'このブラウザではゲームを起動できませんでした。',
    'step.reload': '再読み込み',
    'hint.rom': '：ファイルを選ぶ',
    'rom.missing.parent': 'この zip にはゲームに必要な {file} がありません。スプリットセットなら、残りのファイルは親セットの {parent} にあります。{parent} も追加してください。',
    'rom.missing.more': 'これらの zip にはゲームに必要な {file} がありません。ゲームの zip と {parent} を一緒に選ぶか、マージ済みのセットを選んでください。',
  };

  /* name: the language's name in itself, for the switch. */
  const LANGS = { en: { name: 'English', strings: en }, ja: { name: '日本語', strings: ja } };
  const DEFAULT = 'en';

  /* Other names a visit may give a language: ?lang=jp is Japanese. */
  const ALIASES = { jp: 'ja' };

  const STORE = 'm2hle2.lang';
  const store = {
    get() { try { return localStorage.getItem(STORE); } catch { return null; } },
    set(v) { try { localStorage.setItem(STORE, v); } catch { /* private mode */ } },
  };

  /* Whether this visitor seems to be in Japan: the clock is on Japan time, or
   * Japanese is the browser's first language. */
  function inJapan() {
    let zone = '';
    try { zone = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch { /* old browser */ }
    const first = (navigator.languages && navigator.languages[0]) || navigator.language || '';
    return zone === 'Asia/Tokyo' || /^ja\b/i.test(first);
  }

  const q = new URLSearchParams(location.search);
  /* The language this visit names in its address, or null. */
  const named = q.get('lang') ? ALIASES[q.get('lang')] || q.get('lang') : null;
  if (named) store.set(named);
  /* The language the visit asked for, even one the site has no words in: the
   * board's country follows it (m2hle-game.js), ?lang=fr is Export in English.
   * A guessed Japanese is asked for as ?lang=ja is: Japan's board too. */
  const asked = named || store.get() || (inJapan() ? 'ja' : DEFAULT);
  const lang = LANGS[asked] ? asked : DEFAULT;

  /* The string for `key` in this page's language, {name}s filled from `vars`. */
  function t(key, vars) {
    const s = LANGS[lang].strings[key] ?? en[key] ?? key;
    return vars ? s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? vars[k] : m)) : s;
  }

  /* More keys, { en: {...}, ja: {...} }: a script's own words, kept beside it.
   * English must have every key, as above. */
  function add(tables) {
    for (const [code, strings] of Object.entries(tables)) {
      if (LANGS[code]) Object.assign(LANGS[code].strings, strings);
    }
  }

  /* Translate everything under `root` that names a key. A page that calls this
   * itself, before its scripts fill it in (index.html), is not done again at
   * DOMContentLoaded, which would undo what they wrote. */
  let applied = false;
  /* Translate everything under `root` that names a key. */
  function apply(root) {
    const all = (attr, fn) => root.querySelectorAll('[' + attr + ']')
      .forEach((el) => fn(el, el.getAttribute(attr)));
    all('data-i18n', (el, k) => { el.textContent = t(k); });
    all('data-i18n-html', (el, k) => { el.innerHTML = t(k); });
    all('data-i18n-title', (el, k) => { el.title = t(k); });
    all('data-i18n-aria', (el, k) => el.setAttribute('aria-label', t(k)));
    all('data-i18n-placeholder', (el, k) => { el.placeholder = t(k); });
    all('data-i18n-alt', (el, k) => { el.alt = t(k); });
    if (root === document) applied = true;
  }

  /* `params` (a URLSearchParams) with the language the visit's address named,
   * if it named one, so a link keeps it; a guessed or remembered one is the
   * same on the next page without it. */
  function keep(params) {
    if (named) params.set('lang', named);
    return params;
  }

  /* The query a link to a page with no other parameters needs: '' or '?lang=..'. */
  function query() {
    const q = keep(new URLSearchParams()).toString();
    return q ? '?' + q : '';
  }

  document.documentElement.lang = lang;
  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', () => { if (!applied) apply(document); });
  else apply(document);

  return { LANGS, DEFAULT, lang, asked, named, t, add, apply, keep, query };
})();
