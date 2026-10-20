/*
 * The games this site plays, and which one a visit asked for (Pinboard #881).
 * Loaded first by both pages: home.html draws a box per game from it, and
 * index.html runs the one picked (m2hle-game.js).
 *
 *   arcade.sonicthefighte.rs/?game=sfight&profile=sfight_console
 *
 * ?game= is a key of GAMES, ?profile= one of its profiles (the first is the
 * default, as in src/profiles/registry.h). A game lists every variant MAME
 * knows; one marked `soon` has no profile in the emulator yet, so the front page
 * shows it greyed and ?profile= cannot pick it. Without ?game=, a page named for a
 * game (sfight.html, srallyc.html: web/stamp-site.cmake) or a host named for
 * one (play.*, srally.*: the old sites, now aliases) picks it, and any other
 * query (?follow=, ?rom=, ...) is Sonic the Fighters, as it was before there
 * was a front page. A bare visit, one with only ?lang=, or an unknown ?game=,
 * is the front page.
 *
 * Each game's profiles must be in the web build (registry.h, M2HLE_WEB).
 *
 * A game's words are English; `i18n` holds another language's (m2hle-lang.js,
 * ?lang=), any of title, blurb, romHint, labels, and a profile's name and about
 * by its id. What a language leaves out stays English. text() merges them.
 */
'use strict';

const m2hleGames = (() => {
  const GAMES = {
    sfight: {
      title: 'Sonic the Fighters',
      year: 1996, board: 'Model 2B-CRX', colors: ['#1d4fd8', '#0b1d5c'],
      blurb: 'Sonic and friends fight it out on the Death Egg. Online play over RPCN and GGPO.',
      profiles: [
        { id: 'sfight_console', name: 'Console', about: 'The PS3 release: its menus, free play' },
        { id: 'sfight', name: 'Arcade', about: 'The cabinet as it was' },
      ],
      romHint: 'A merged <code>sfight.zip</code> or <code>schamp.zip</code>, or <code>sfight.zip</code> and <code>schamp.zip</code> together',
      labels: {},
      i18n: {
        ja: {
          title: 'ソニック・ザ・ファイターズ',
          blurb: 'ソニックたちがデスエッグで大乱闘。RPCN と GGPO でオンライン対戦。',
          profiles: { sfight_console: { name: 'コンソール', about: 'PS3 版：メニュー付き、フリープレイ' },
                      sfight: { name: 'アーケード', about: '筐体そのまま' } },
          romHint: 'マージ済みの <code>sfight.zip</code> または <code>schamp.zip</code>、もしくは <code>sfight.zip</code> と <code>schamp.zip</code> を一緒に',
        },
      },
    },
    srallyc: {
      title: 'Sega Rally Championship',
      year: 1995, board: 'Model 2A-CRX', colors: ['#d8361d', '#5c1a0b'],
      blurb: 'Desert, forest, mountain and lakeside: the rally that started it all.',
      profiles: [
        { id: 'srallyc', name: 'Rev C', about: 'Twin/DX, Revision C' },
        { id: 'srallycb', name: 'Rev B', about: 'Twin/DX, Revision B', soon: true },
        { id: 'srallycc', name: 'Rev A', about: 'Twin/DX, Revision A', soon: true },
        { id: 'srallycdx', name: 'DX Rev A', about: 'DX (single cabinet), Revision A', soon: true },
        { id: 'srallycdxa', name: 'DX', about: 'DX (single cabinet)', soon: true },
      ],
      romHint: 'Your <code>srallyc.zip</code> (Sega Rally Championship, Twin/DX)',
      /* src/profiles/srallyc.h: LEFT/RIGHT the wheel, B1 accelerator, B2 brake,
       * UP/DOWN a gear, B3 VIEW CHANGE. */
      labels: { up: 'Shift up', down: 'Shift down', left: 'Steer left', right: 'Steer right',
                b1: 'Accelerator', b2: 'Brake', b3: 'View change', b4: 'Unused' },
      /* No replay format of its own: a replay is always the buttons from the
       * game's start (src/core/replay.h, replay__saves_inputs). */
      ownReplay: false,
      i18n: {
        ja: {
          title: 'セガラリーチャンピオンシップ',
          blurb: 'デザート、フォレスト、マウンテン、レイクサイド。すべてはここから始まった。',
          profiles: { srallyc: { about: 'ツイン/DX、リビジョン C' }, srallycb: { about: 'ツイン/DX、リビジョン B' },
                      srallycc: { about: 'ツイン/DX、リビジョン A' },
                      srallycdx: { about: 'DX（シングル筐体）、リビジョン A' }, srallycdxa: { about: 'DX（シングル筐体）' } },
          romHint: 'お持ちの <code>srallyc.zip</code>（セガラリーチャンピオンシップ ツイン/DX）',
          labels: { up: 'シフトアップ', down: 'シフトダウン', left: 'ハンドル左', right: 'ハンドル右',
                    b1: 'アクセル', b2: 'ブレーキ', b3: 'ビューチェンジ', b4: '未使用' },
        },
      },
    },
    vonj: {
      title: 'Cyber Troopers Virtual-On',
      year: 1995, board: 'Model 2B-CRX', colors: ['#2a9d8f', '#0b3d3a'],
      blurb: 'Giant robots, two sticks each, one arena. The Japanese release.',
      profiles: [
        { id: 'vonj', name: 'Japan', about: 'Twin, Revision B (Japan)' },
        { id: 'von', name: 'Export', about: 'Twin (Export)', soon: true },
        { id: 'vonu', name: 'USA', about: 'Twin (USA, Revision B)', soon: true },
        { id: 'vonr', name: 'Relay', about: 'Relay (Japan)', soon: true },
      ],
      /* vonj is a clone: its zip and its parent's, picked together, or a merged one. */
      romHint: 'Your <code>vonj.zip</code> and its parent <code>von.zip</code>, picked together, or a merged <code>vonj.zip</code>',
      /* src/profiles/vonj.h: the left lever is UP/DOWN/LEFT/RIGHT, B1 its
       * trigger, B2 its dash; B3 both triggers, B4 both dashes. The right lever
       * is player 2's directions and B1 / B2, which a pad plays as rows of its
       * own (`twin`, m2hle-pad.js), and a Saturn Twin Stick on a Bliss-Box whole. */
      twin: true,
      labels: { up: 'Left lever up', down: 'Left lever down', left: 'Left lever left', right: 'Left lever right',
                rup: 'Right lever up', rdown: 'Right lever down', rleft: 'Right lever left', rright: 'Right lever right',
                b1: 'Left shot', b2: 'Left dash', rshot: 'Right shot', rdash: 'Right dash',
                b3: 'Both shots', b4: 'Both dashes' },
      ownReplay: false,
      /* No shared board or ranking for it (arcade/games.mjs); its save is kept
       * in the browser (m2hle-page.js, web_backup_keep). */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: '電脳戦機バーチャロン',
          blurb: '巨大ロボット、ツインスティック、ひとつの戦場。日本版。',
          profiles: { vonj: { name: '日本', about: 'ツイン、リビジョン B（日本）' },
                      von: { name: '海外', about: 'ツイン（海外）' },
                      vonu: { name: '北米', about: 'ツイン（北米、リビジョン B）' },
                      vonr: { name: 'リレー', about: 'リレー（日本）' } },
          romHint: 'お持ちの <code>vonj.zip</code> と親セットの <code>von.zip</code> を一緒に選ぶか、マージ済みの <code>vonj.zip</code>',
          labels: { up: '左レバー上', down: '左レバー下', left: '左レバー左', right: '左レバー右',
                    rup: '右レバー上', rdown: '右レバー下', rleft: '右レバー左', rright: '右レバー右',
                    b1: '左ショット', b2: '左ダッシュ', rshot: '右ショット', rdash: '右ダッシュ',
                    b3: '両ショット', b4: '両ダッシュ' },
        },
      },
    },
    dynamcop: {
      title: 'Dynamite Cop',
      year: 1998, board: 'Model 2A/2B/2C-CRX', colors: ['#e0a106', '#5c3a0b'],
      blurb: 'Pirates have taken the President\'s daughter hostage on a cruise liner. Bruno Delinger is going in.',
      /* src/profiles/dynamcop.h: the 2B sets first, which track MAME closest;
       * the 2A sets are the same game on the TGP board, the 2C on the TGPx4. */
      profiles: [
        { id: 'dynamcopb', name: 'Export', about: 'Model 2B (Export)' },
        { id: 'dyndeka2b', name: 'Japan', about: 'Dynamite Deka 2, Model 2B (Japan)' },
        { id: 'dynamcop', name: 'Export 2A', about: 'Model 2A (Export)' },
        { id: 'dyndeka2', name: 'Japan 2A', about: 'Dynamite Deka 2, Model 2A (Japan)' },
        { id: 'dynamcopc', name: 'USA 2C', about: 'Model 2C (USA)' },
      ],
      /* Every set but dynamcop is a clone of dynamcop.zip. */
      romHint: 'Your <code>dynmcopb.zip</code>, <code>dyndek2b.zip</code>, <code>dyndeka2.zip</code> or <code>dynmcopc.zip</code> and its parent <code>dynamcop.zip</code>, picked together, or <code>dynamcop.zip</code> alone',
      /* src/profiles/dynamcop.h: an 8-way stick, B1 punch, B2 kick, B3 jump. */
      labels: { b1: 'Punch', b2: 'Kick', b3: 'Jump', b4: 'Unused' },
      ownReplay: false,
      /* No shared board or ranking (arcade/games.mjs); its save stays in the browser. */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: 'ダイナマイト刑事2',
          blurb: '豪華客船を乗っ取った海賊に大統領の娘が人質に。ブルーノ・デリンジャー、出動。',
          profiles: { dynamcopb: { name: '海外', about: 'Model 2B（海外）' },
                      dyndeka2b: { name: '日本', about: 'ダイナマイト刑事2、Model 2B（日本）' },
                      dynamcop: { name: '海外 2A', about: 'Model 2A（海外）' },
                      dyndeka2: { name: '日本 2A', about: 'ダイナマイト刑事2、Model 2A（日本）' },
                      dynamcopc: { name: '北米 2C', about: 'Model 2C（北米）' } },
          romHint: 'お持ちの <code>dynmcopb.zip</code>、<code>dyndek2b.zip</code>、<code>dyndeka2.zip</code> または <code>dynmcopc.zip</code> と親セットの <code>dynamcop.zip</code> を一緒に選ぶか、<code>dynamcop.zip</code> だけ',
          labels: { b1: 'パンチ', b2: 'キック', b3: 'ジャンプ', b4: '未使用' },
        },
      },
    },
    fvipers: {
      title: 'Fighting Vipers',
      year: 1995, board: 'Model 2B-CRX', colors: ['#c81d6a', '#3d0b2a'],
      blurb: 'Armoured street fighters in caged arenas: break the armour, then the walls.',
      profiles: [
        { id: 'fvipers', name: 'Rev D', about: 'Revision D' },
        { id: 'fvipersb', name: 'Rev B', about: 'Revision B', soon: true },
      ],
      romHint: 'Your <code>fvipers.zip</code> (Fighting Vipers, Revision D)',
      /* src/profiles/fvipers.h: MAME's INPUT_PORTS(vf2), B1 punch, B2 kick, B3 guard. */
      labels: { b1: 'Punch', b2: 'Kick', b3: 'Guard', b4: 'Unused' },
      ownReplay: false,
      /* No shared board or ranking (arcade/games.mjs); its save stays in the browser. */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: 'ファイティングバイパーズ',
          blurb: 'アーマーをまとった喧嘩屋たちが金網の中で激突。アーマーを砕き、壁をぶち破れ。',
          profiles: { fvipers: { name: 'リビジョン D', about: 'リビジョン D' },
                      fvipersb: { name: 'リビジョン B', about: 'リビジョン B' } },
          romHint: 'お持ちの <code>fvipers.zip</code>（ファイティングバイパーズ、リビジョン D）',
          labels: { b1: 'パンチ', b2: 'キック', b3: 'ガード', b4: '未使用' },
        },
      },
    },
  };
  const HOSTS = { play: 'sfight', srally: 'srallyc' };

  /* { id, profile } for this visit, or null for the front page. */
  function pick(loc) {
    const q = new URLSearchParams(loc.search);
    const page = (/\/([a-z0-9_]+)\.html$/.exec(loc.pathname) || [])[1];
    const host = (/^([a-z]+)\./.exec(loc.hostname) || [])[1];
    let id = null;
    if (q.has('game')) id = GAMES[q.get('game')] ? q.get('game') : null;
    else if (GAMES[page]) id = page;
    else if (HOSTS[host]) id = HOSTS[host];
    else if ([...q.keys()].some((k) => k !== 'lang')) id = 'sfight';
    if (!id) return null;
    const profiles = GAMES[id].profiles;
    const profile = profiles.find((p) => p.id === q.get('profile') && !p.soon) || profiles[0];
    return { id, profile: profile.id };
  }

  /* The link that plays a game, relative to the site's root, in this page's
   * language. */
  function href(id, profile) {
    const q = new URLSearchParams({ game: id, profile: profile || GAMES[id].profiles[0].id });
    return './?' + m2hleLang.keep(q);
  }

  /* A game's entry in `lang` (this page's by default): its i18n words over the
   * English ones, labels and profiles key by key. */
  function text(id, lang) {
    const g = GAMES[id];
    const tr = (g.i18n || {})[lang || m2hleLang.lang] || {};
    const own = tr.profiles || {};
    return Object.assign({}, g, tr, {
      labels: Object.assign({}, g.labels, tr.labels),
      profiles: g.profiles.map((p) => Object.assign({}, p, own[p.id])),
    });
  }

  return { GAMES, pick, href, text, picked: pick(location) };
})();
