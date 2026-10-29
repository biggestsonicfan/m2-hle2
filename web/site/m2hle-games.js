/*
 * The games this site plays, and which one a visit asked for (Pinboard #881).
 * Loaded first by both pages: home.html draws a box per game from it, and
 * index.html runs the one picked (m2hle-game.js).
 *
 *   arcade.sonicthefighte.rs/?game=sfight&profile=sfight_console
 *
 * ?game= is a key of GAMES, ?profile= one of its profiles (the first is the
 * default, as in src/profiles/registry.h; a game's `first` can name another
 * for a language, Virtual-On's Japanese game on the Japanese page). A game lists every variant MAME
 * knows; one marked `soon` has no profile in the emulator yet, so the front page
 * shows it greyed and ?profile= cannot pick it. Without ?game=, a page named for a
 * game (sfight.html, srallyc.html: web/stamp-site.cmake) or a host named for
 * one (play.*, srally.*: the old sites, now aliases) picks it, and any other
 * query (?follow=, ?rom=, ...) is Sonic the Fighters, as it was before there
 * was a front page. A bare visit, one with only ?lang=, or an unknown ?game=,
 * is the front page.
 *
 * Each game's profiles must be in the web build (registry.h, M2HLE_WEB). A
 * site profile can also be one of them booted in a fixed country: `runs` names
 * the emulator's profile and `region` the --region= it gets over the page's
 * language (m2hle-game.js; Fighting Vipers' Japanese game, Pinboard #1350).
 *
 * A game's words are English; `i18n` holds another language's (m2hle-lang.js,
 * ?lang=), any of title, blurb, romHint, labels, and a profile's name and about
 * by its id. What a language leaves out stays English. text() merges them.
 */
'use strict';

const m2hleGames = (() => {
  /* A racing cabinet's keys over m2hle-keys.js's defaults, m2emulator's layout
   * as on the desktop (src/board/input.h INPUT_KEYS_DRIVING, Pinboard #1347):
   * the arrows drive (Up the accelerator, Down the brake), Z X C V the view
   * buttons (player 2's B3 / B4 are Daytona's VR3 / VR4), Q / W the shifts,
   * A S D F the gears 1-4 and G neutral. */
  const DRIVING_KEYS = {
    p1: { up: ['KeyW'], down: ['KeyQ'], b1: ['ArrowUp'], b2: ['ArrowDown'], b3: ['KeyZ'], b4: ['KeyX'],
          gear1: ['KeyA'], gear2: ['KeyS'], gear3: ['KeyD'], gear4: ['KeyF'], gearn: ['KeyG'] },
    p2: { b3: ['KeyC'], b4: ['KeyV'] },
  };

  const GAMES = {
    sfight: {
      title: 'Sonic the Fighters',
      year: 1996, board: 'Model 2B-CRX', colors: ['#1d4fd8', '#0b1d5c'],
      blurb: 'Sonic and friends fight it out on the Death Egg. Online play over RPCN and GGPO.',
      profiles: [
        { id: 'sfight_console', name: 'Console', about: 'The PS3 release: its menus, free play' },
        { id: 'sfight', name: 'Arcade', about: 'The cabinet as it was' },
        { id: 'schamp', name: 'Championship', about: 'Sonic Championship, the USA release' },
      ],
      romHint: 'A merged <code>sfight.zip</code> or <code>schamp.zip</code>, or <code>sfight.zip</code> and <code>schamp.zip</code> together',
      labels: {},
      i18n: {
        ja: {
          title: 'ソニック・ザ・ファイターズ',
          blurb: 'ソニックたちがデスエッグで大乱闘。RPCN と GGPO でオンライン対戦。',
          profiles: { sfight_console: { name: 'コンソール', about: 'PS3 版：メニュー付き、フリープレイ' },
                      sfight: { name: 'アーケード', about: '筐体そのまま' },
                      schamp: { name: 'チャンピオンシップ', about: 'ソニック・チャンピオンシップ、北米版' } },
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
      keys: DRIVING_KEYS,
      gears: true,                         /* a button per gate as well (io_5649.h) */
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
      blurb: 'Giant robots, two sticks each, one arena.',
      /* von is the English game, the USA Revision B program in von.zip
       * (src/profiles/vonj.h, Pinboard #1342), and vonj the Japanese one, the
       * Japanese page's first. MAME's current von.zip holds the Export program
       * instead, which m2-hle2 does not run yet. */
      profiles: [
        { id: 'von', name: 'USA', about: 'Twin, Revision B (USA)' },
        { id: 'vonj', name: 'Japan', about: 'Twin, Revision B (Japan)' },
        { id: 'vonr', name: 'Relay', about: 'Relay (Japan)', soon: true },
      ],
      first: { ja: 'vonj' },
      /* vonj is a clone: its zip and its parent's, picked together, or a merged one. */
      romHint: 'Your <code>von.zip</code> (USA); for Japan, <code>vonj.zip</code> and its parent <code>von.zip</code> picked together, or a merged <code>vonj.zip</code>',
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
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: '電脳戦機バーチャロン',
          blurb: '巨大ロボット、ツインスティック、ひとつの戦場。',
          profiles: { vonj: { name: '日本', about: 'ツイン、リビジョン B（日本）' },
                      von: { name: '北米', about: 'ツイン、リビジョン B（北米）' },
                      vonr: { name: 'リレー', about: 'リレー（日本）' } },
          romHint: 'お持ちの <code>vonj.zip</code> と親セットの <code>von.zip</code> を一緒に選ぶか、マージ済みの <code>vonj.zip</code>（北米版は <code>von.zip</code> だけ）',
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
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
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
        /* The same set and profile booted as the Japanese game on any page
         * (src/profiles/fvipers.h's COUNTRY hooks, Pinboard #1350); the
         * Japanese page boots fvipers as that already. */
        { id: 'fvipers_japan', runs: 'fvipers', region: 'japan', name: 'Rev D Japan',
          about: 'Revision D, the Japanese game' },
        { id: 'fvipersb', name: 'Rev B', about: 'Revision B', soon: true },
      ],
      romHint: 'Your <code>fvipers.zip</code> (Fighting Vipers, Revision D)',
      /* src/profiles/fvipers.h: MAME's INPUT_PORTS(vf2), B1 punch, B2 kick, B3 guard. */
      labels: { b1: 'Punch', b2: 'Kick', b3: 'Guard', b4: 'Unused' },
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'ファイティングバイパーズ',
          blurb: 'アーマーをまとった喧嘩屋たちが金網の中で激突。アーマーを砕き、壁をぶち破れ。',
          profiles: { fvipers: { name: 'リビジョン D', about: 'リビジョン D' },
                      fvipers_japan: { name: 'リビジョン D 日本', about: 'リビジョン D（日本版）' },
                      fvipersb: { name: 'リビジョン B', about: 'リビジョン B' } },
          romHint: 'お持ちの <code>fvipers.zip</code>（ファイティングバイパーズ、リビジョン D）',
          labels: { b1: 'パンチ', b2: 'キック', b3: 'ガード', b4: '未使用' },
        },
      },
    },
    hotd: {
      title: 'The House of the Dead',
      year: 1996, board: 'Model 2C-CRX', colors: ['#8c1010', '#1c0606'],
      blurb: 'Dr. Curien\'s mansion is overrun. Two agents, two guns, and the dead between them and the truth.',
      /* src/profiles/hotd.h: Revision A or the first release (MAME's hotdo), and
       * since #1286 the first release and the prototype as sets of their own. */
      profiles: [
        { id: 'hotd', name: 'Rev A', about: 'Revision A, or the first release' },
        { id: 'hotdo', name: 'First', about: 'The first release' },
        { id: 'hotdp', name: 'Prototype', about: 'The prototype, in Japanese only' },
      ],
      romHint: 'Your <code>hotd.zip</code> (The House of the Dead), or <code>hotdo.zip</code> or <code>hotdp.zip</code> and its parent <code>hotd.zip</code>, picked together',
      /* A light gun on the mouse or a finger (main_web.c, web_gun_event); the
       * pad's directions walk it, B1 fires, B2 reloads (board/lightgun.h). */
      labels: { b1: 'Trigger', b2: 'Reload', b3: 'Unused', b4: 'Unused' },
      /* The Controls panel offers the flash on each shot (m2hle-pad.js, lightgun_host.h). */
      gun: true,
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'ザ・ハウス・オブ・ザ・デッド',
          blurb: 'キュリアン博士の館は死者であふれた。二人のエージェント、二丁の銃、そして真相を阻む死者たち。',
          profiles: { hotd: { name: 'リビジョン A', about: 'リビジョン A または初期版' },
                      hotdo: { name: '初期版', about: '最初のリリース' },
                      hotdp: { name: 'プロトタイプ', about: 'プロトタイプ（日本語のみ）' } },
          romHint: 'お持ちの <code>hotd.zip</code>（ザ・ハウス・オブ・ザ・デッド）、または <code>hotdo.zip</code>、<code>hotdp.zip</code> と親セットの <code>hotd.zip</code> を一緒に',
          labels: { b1: 'トリガー', b2: 'リロード', b3: '未使用', b4: '未使用' },
        },
      },
    },
    daytona: {
      title: 'Daytona USA',
      year: 1994, board: 'Model 2', colors: ['#1d8fd8', '#0b2a5c'],
      blurb: 'Three-Seven Speedway, Dinosaur Canyon, Seaside Street Galaxy. Rolling start!',
      profiles: [
        { id: 'daytona', name: 'Rev A', about: 'Japan, Revision A' },
        /* The clones of #1286, each a clone of daytona.zip; MAME marks the hacks not working. */
        { id: 'daytona93', name: 'Original', about: 'The 1993 first release' },
        { id: 'daytonase', name: 'Special', about: 'Special Edition, Revision A' },
        { id: 'daytonas', name: 'Saturn ads', about: 'With the Sega Saturn adverts' },
        { id: 'daytonat', name: 'Turbo 1', about: 'Turbo hack, set 1' },
        { id: 'daytonata', name: 'Turbo 2', about: 'Turbo hack, set 2' },
        { id: 'daytonagtx', name: 'GTX 2004', about: 'GTX 2004 Edition (a hack)' },
      ],
      /* The I/O board's program is MAME's model1io device (src/profiles/daytona.h). */
      romHint: 'Your <code>daytona.zip</code> and the I/O board\'s <code>model1io.zip</code>, picked together; for another build, its own zip (<code>daytona93.zip</code>, <code>daytonase.zip</code>, ...) with those two',
      /* src/profiles/daytona.h: LEFT/RIGHT the wheel, UP/DOWN the four-speed
       * lever, B1 accelerator, B2 brake, B3/B4 VR1/VR2 (player 2's B3/B4 are
       * VR3/VR4). The wheel's force is the pad's rumble (m2hle-pad.js). */
      labels: { up: 'Shift up', down: 'Shift down', left: 'Steer left', right: 'Steer right',
                b1: 'Accelerator', b2: 'Brake', b3: 'View 1', b4: 'View 2' },
      keys: DRIVING_KEYS,
      /* A button per gate of the lever as well as Shift up / down (io_5649.h). */
      gears: true,
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'デイトナUSA',
          blurb: 'スリーセブン・スピードウェイ、ダイナソー・キャニオン、シーサイド・ストリート・ギャラクシー。ローリングスタート！',
          profiles: { daytona: { name: 'リビジョン A', about: '日本、リビジョン A' },
                      daytona93: { name: '初期版', about: '1993 年の最初のリリース' },
                      daytonase: { name: 'スペシャル', about: 'スペシャルエディション、リビジョン A' },
                      daytonas: { name: 'サターン広告', about: 'セガサターンの広告入り' },
                      daytonat: { name: 'ターボ 1', about: 'ターボハック、セット 1' },
                      daytonata: { name: 'ターボ 2', about: 'ターボハック、セット 2' },
                      daytonagtx: { name: 'GTX 2004', about: 'GTX 2004 エディション（ハック版）' } },
          romHint: 'お持ちの <code>daytona.zip</code> と I/O ボードの <code>model1io.zip</code> を一緒に。別のバージョンは、その zip（<code>daytona93.zip</code>、<code>daytonase.zip</code> など）もこの二つと一緒に',
          labels: { up: 'シフトアップ', down: 'シフトダウン', left: 'ハンドル左', right: 'ハンドル右',
                    b1: 'アクセル', b2: 'ブレーキ', b3: '視点 1', b4: '視点 2' },
        },
      },
    },
    vcop: {
      title: 'Virtua Cop',
      year: 1994, board: 'Model 2', colors: ['#d8b01d', '#14204c'],
      blurb: 'Rage and Smarty of the Virtua City police take on EVL Inc. The sight turns red on whoever is about to shoot.',
      /* src/profiles/vcop.h: Revision B, one profile. */
      profiles: [
        { id: 'vcop', name: 'Rev B', about: 'Revision B' },
      ],
      /* The I/O board's program is MAME's model1io2 device (src/profiles/vcop.h). */
      romHint: 'Your <code>vcop.zip</code> and the I/O board\'s <code>model1io2.zip</code>, picked together',
      /* The House of the Dead's gun (board/lightgun.h): B1 fires, B2 reloads. */
      labels: { b1: 'Trigger', b2: 'Reload', b3: 'Unused', b4: 'Unused' },
      gun: true,
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'バーチャコップ',
          blurb: 'バーチャシティ警察のレイジとスマーティが EVL 社に立ち向かう。撃とうとする相手の照準が赤く光る。',
          profiles: { vcop: { name: 'リビジョン B', about: 'リビジョン B' } },
          romHint: 'お持ちの <code>vcop.zip</code> と I/O ボードの <code>model1io2.zip</code> を一緒に',
          labels: { b1: 'トリガー', b2: 'リロード', b3: '未使用', b4: '未使用' },
        },
      },
    },
    vf2: {
      title: 'Virtua Fighter 2',
      year: 1994, board: 'Model 2A-CRX', colors: ['#1d8fc8', '#0b2a3d'],
      blurb: 'Akira, Sarah, Lau and the rest, joined by Lion and Shun Di: the fighter that set the pace for 3D.',
      /* src/profiles/vf2.h: Version 2.1 and its three clones, each a clone of vf2.zip. */
      profiles: [
        { id: 'vf2', name: 'Version 2.1', about: 'Version 2.1' },
        { id: 'vf2b', name: 'Rev B', about: 'Revision B' },
        { id: 'vf2a', name: 'Rev A', about: 'Revision A' },
        { id: 'vf2o', name: 'Original', about: 'The first release' },
      ],
      romHint: 'Your <code>vf2.zip</code> (Version 2.1), or <code>vf2b.zip</code>, <code>vf2a.zip</code> or <code>vf2o.zip</code> and its parent <code>vf2.zip</code>, picked together',
      /* src/profiles/vf2.h: MAME's INPUT_PORTS(vf2), B1 punch, B2 kick, B3 guard. */
      labels: { b1: 'Punch', b2: 'Kick', b3: 'Guard', b4: 'Unused' },
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'バーチャファイター2',
          blurb: 'アキラ、サラ、ラウたちに、リオンとシュンディが加わった。3D 格闘の基準を作った一作。',
          profiles: { vf2: { name: 'Ver.2.1', about: 'Ver.2.1' }, vf2b: { name: 'リビジョン B', about: 'リビジョン B' },
                      vf2a: { name: 'リビジョン A', about: 'リビジョン A' }, vf2o: { name: '初期版', about: '最初のリリース' } },
          romHint: 'お持ちの <code>vf2.zip</code>（Ver.2.1）、または <code>vf2b.zip</code>、<code>vf2a.zip</code>、<code>vf2o.zip</code> と親セットの <code>vf2.zip</code> を一緒に',
          labels: { b1: 'パンチ', b2: 'キック', b3: 'ガード', b4: '未使用' },
        },
      },
    },
    doa: {
      title: 'Dead or Alive',
      year: 1996, board: 'Model 2A/2B-CRX', colors: ['#c8102e', '#2a0a0e'],
      blurb: 'Kasumi, Zack, Tina and the rest enter the DOA tournament. Hold to reverse, and mind the danger zone.',
      /* src/profiles/doa.h: the 2B sets first, then the 2A ones (the same game
       * on the TGP board); doaae is locked to Export. Every set but doa is a
       * clone of doa.zip. */
      profiles: [
        { id: 'doa', name: 'Rev C', about: 'Model 2B, Revision C' },
        { id: 'doab', name: 'Rev B', about: 'Model 2B, Revision B' },
        { id: 'doaa', name: 'Rev A 2A', about: 'Model 2A, Revision A' },
        { id: 'doaab', name: '2A', about: 'Model 2A, the first release' },
        { id: 'doaae', name: 'Export 2A', about: 'Model 2A, Export, Revision A' },
      ],
      romHint: 'Your <code>doa.zip</code> (Revision C), or <code>doab.zip</code>, <code>doaa.zip</code>, <code>doaab.zip</code> or <code>doaae.zip</code> and its parent <code>doa.zip</code>, picked together',
      /* src/profiles/doa.h: MAME's INPUT_PORTS(doa), B1 hold, B2 punch, B3 kick. */
      labels: { b1: 'Hold', b2: 'Punch', b3: 'Kick', b4: 'Unused' },
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'デッド オア アライブ',
          blurb: 'かすみ、ザック、ティナたちが DOA 大会に挑む。ホールドで切り返し、デンジャーゾーンに気をつけろ。',
          profiles: { doa: { name: 'リビジョン C', about: 'Model 2B、リビジョン C' },
                      doab: { name: 'リビジョン B', about: 'Model 2B、リビジョン B' },
                      doaa: { name: 'リビジョン A 2A', about: 'Model 2A、リビジョン A' },
                      doaab: { name: '2A', about: 'Model 2A、最初のリリース' },
                      doaae: { name: '海外 2A', about: 'Model 2A、海外、リビジョン A' } },
          romHint: 'お持ちの <code>doa.zip</code>（リビジョン C）、または <code>doab.zip</code>、<code>doaa.zip</code>、<code>doaab.zip</code>、<code>doaae.zip</code> と親セットの <code>doa.zip</code> を一緒に',
          labels: { b1: 'ホールド', b2: 'パンチ', b3: 'キック', b4: '未使用' },
        },
      },
    },
    airwlkrs: {
      title: 'Air Walkers',
      year: 1997, board: 'Model 2A-CRX', colors: ['#2fa8c8', '#0b3a4c'],
      blurb: 'Data East\'s basketball: sixteen cities, team select, tip-off. Up to four players: pads 3 and 4 play once the test menu\'s PLAYER SELECTION is 4P.',
      profiles: [
        { id: 'airwlkrs', name: 'Arcade', about: 'The only build MAME knows' },
      ],
      romHint: 'Your <code>airwlkrs.zip</code> (Air Walkers)',
      /* src/profiles/airwlkrs.h: MAME's INPUT_PORTS(airwlkrs), three buttons a
       * player. Players 3 and 4 (port F) are the third and fourth pad
       * (m2hle-pad.js), outside a session. */
      labels: { b4: 'Unused' },
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          blurb: 'データイーストのバスケットボール。16都市からチームを選んでティップオフ。テストメニューの PLAYER SELECTION を 4P にすれば、3・4台目のパッドで4人まで遊べる。',
          profiles: { airwlkrs: { name: 'アーケード', about: 'MAME が知る唯一の版' } },
          romHint: 'お持ちの <code>airwlkrs.zip</code>（エアーウォーカーズ）',
          labels: { b4: '未使用' },
        },
      },
    },
    powsled: {
      title: 'Power Sled',
      year: 1996, board: 'Model 2B-CRX', colors: ['#e05a1d', '#4c1a0b'],
      blurb: 'Snow sleds on a linked ring of cabinets. Press Entry to race alone: the Main unit (the operator\'s) runs beside your sled in this tab. For a race against a friend, open a Room Match: its owner runs the Main and its guest rides.',
      profiles: [
        { id: 'powsled', name: 'Slave', about: 'The ride, Revision A (and the Main, in a room)' },
        { id: 'powsledr', name: 'Relay', about: 'The big screen (Relay unit)', soon: true },
      ],
      romHint: 'Your <code>powsled.zip</code> (Power Sled, the Slave unit)',
      /* src/profiles/powsled.h: the ride steers with its two pedals (LEFT and
       * RIGHT), B1 is the rider's Entry, B2 Call; B3 cancels the network check,
       * B4 an error. On the Main (a room's owner, net/comm_role.h) the same
       * buttons are the operator's panel: P1 B2 opens entry, P2 B2 twice and
       * P2 B3 start the race (README.md, Power Sled). */
      labels: { left: 'Left pedal', right: 'Right pedal', up: 'Main: panel button 3', down: 'Unused',
                b1: 'Entry', b2: 'Call', b3: 'Cancel network check / Main: start race', b4: 'Cancel error' },
      ownReplay: false,
      /* No shared board: Power Sled keeps no ranking in its backup RAM
       * (scores.sonicthefighte.rs, Pinboard #1559); its save stays in the browser. */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: 'パワースレッド',
          blurb: '筐体をリンクして走るスノースレッド。エントリーを押せば一人でレース：メインユニット（オペレーター側）がこのタブでソリと一緒に動きます。友だちとはルームマッチで：オーナーがメイン、ゲストがソリに乗ります。',
          profiles: { powsled: { name: 'スレーブ', about: 'ライド、リビジョン A（ルームではメインにも）' },
                      powsledr: { name: 'リレー', about: '大画面（リレーユニット）' } },
          romHint: 'お持ちの <code>powsled.zip</code>（パワースレッド、スレーブユニット）',
          labels: { left: '左ペダル', right: '右ペダル', up: 'メイン：パネルボタン3', down: '未使用',
                    b1: 'エントリー', b2: 'コール', b3: 'ネットワークチェック取消／メイン：レース開始', b4: 'エラー取消' },
        },
      },
    },
    lastbrnx: {
      title: 'Last Bronx',
      year: 1996, board: 'Model 2B-CRX', colors: ['#b8862b', '#1c1a24'],
      blurb: 'Eight gang leaders settle Tokyo\'s streets with weapons: tonfa, nunchaku, sai, a bokken, a sledgehammer and more.',
      /* src/profiles/lastbrnx.h: one program pair per country, the others
       * clones of lastbrnx.zip. The Japanese page boots Tokyo Bangaichi. */
      first: { ja: 'lastbrnxj' },
      profiles: [
        { id: 'lastbrnx', name: 'Export', about: 'Export, Revision A' },
        { id: 'lastbrnxu', name: 'USA', about: 'USA, Revision A' },
        { id: 'lastbrnxj', name: 'Japan', about: 'Tokyo Bangaichi, Japan, Revision A' },
      ],
      romHint: 'Your <code>lastbrnx.zip</code> (Export), or <code>lastbrnxu.zip</code> or <code>lastbrnxj.zip</code> (once <code>lastbrnj.zip</code>) and its parent <code>lastbrnx.zip</code>, picked together',
      /* MAME's INPUT_PORTS(vf2), which the game's input test names the same:
       * B1 punch, B2 kick, B3 guard. */
      labels: { b1: 'Punch', b2: 'Kick', b3: 'Guard', b4: 'Unused' },
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'ラストブロンクス 東京番外地',
          blurb: '8人のチームリーダーが武器で東京の街の決着をつける。トンファー、ヌンチャク、釵、木刀、ハンマーほか。',
          profiles: { lastbrnx: { name: '海外版', about: '海外、リビジョン A' },
                      lastbrnxu: { name: 'USA', about: 'USA、リビジョン A' },
                      lastbrnxj: { name: '日本版', about: '東京番外地、日本、リビジョン A' } },
          romHint: 'お持ちの <code>lastbrnx.zip</code>（海外版）、または <code>lastbrnxu.zip</code>、<code>lastbrnxj.zip</code>（旧名 <code>lastbrnj.zip</code>）と親セットの <code>lastbrnx.zip</code> を一緒に',
          labels: { b1: 'パンチ', b2: 'キック', b3: 'ガード', b4: '未使用' },
        },
      },
    },
    pltkids: {
      title: 'Pilot Kids',
      year: 1998, board: 'Model 2A/2B-CRX', colors: ['#e0461c', '#1a2030'],
      blurb: 'Psikyo\'s shoot-\'em-up with toy planes: two kid pilots fly over desks and bookshelves. Shot and bomb, one or two players.',
      /* src/profiles/pltkids.h: the 2B Revision A first, then the 2A build,
       * a clone of pltkids.zip. */
      profiles: [
        { id: 'pltkids', name: 'Rev A', about: 'Model 2B, Revision A' },
        { id: 'pltkidsa', name: '2A', about: 'Model 2A' },
      ],
      romHint: 'Your <code>pltkids.zip</code> (Model 2B), or <code>pltkidsa.zip</code> and its parent <code>pltkids.zip</code>, picked together',
      /* MAME's INPUT_PORTS(pltkids): B1 shot, B2 bomb. */
      labels: { b1: 'Shot', b2: 'Bomb', b3: 'Unused', b4: 'Unused' },
      ownReplay: false,
      /* No shared board: Pilot Kids clears its ranking at every boot
       * (scores.sonicthefighte.rs, Pinboard #1559); its save stays in the browser. */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: 'パイロットキッズ',
          blurb: '彩京のシューティング。2人の子どもパイロットがおもちゃの飛行機で机や本棚の上を飛び回る。ショットとボム、1人または2人プレイ。',
          profiles: { pltkids: { name: 'リビジョン A', about: 'Model 2B、リビジョン A' },
                      pltkidsa: { name: '2A', about: 'Model 2A' } },
          romHint: 'お持ちの <code>pltkids.zip</code>（Model 2B）、または <code>pltkidsa.zip</code> と親セットの <code>pltkids.zip</code> を一緒に',
          labels: { b1: 'ショット', b2: 'ボム', b3: '未使用', b4: '未使用' },
        },
      },
    },
    motoraid: {
      title: 'Motor Raid',
      year: 1997, board: 'Model 2A-CRX', colors: ['#d8a21d', '#2a1608'],
      blurb: 'Motorbike racing with fists and boots: punch and kick the other riders off the road. Races alone; for a race against a friend, open a Room Match: its owner\'s bike is the link MASTER cabinet and its guest\'s the SLAVE.',
      /* src/profiles/motoraid.h: the Twin cabinet, set to STAND ALONE; a
       * room links two tabs (net/comm_role.h), each one cabinet. */
      profiles: [
        { id: 'motoraid', name: 'Twin', about: 'Twin cabinet' },
        { id: 'motoraiddx', name: 'Twin/DX', about: 'Twin/DX', soon: true },
      ],
      romHint: 'Your <code>motoraid.zip</code> (Motor Raid - Twin)',
      /* MAME's INPUT_PORTS(motoraid): the throttle, brake and bank are the
       * 315-5649's analog channels (io_5649.h, quirks.wheel.bike), B3 Punch,
       * B4 Kick. The arrows ride, as in the desktop's driving keys. */
      labels: { up: 'Unused', down: 'Unused', left: 'Lean left', right: 'Lean right',
                b1: 'Throttle', b2: 'Brake', b3: 'Punch', b4: 'Kick' },
      keys: DRIVING_KEYS,
      ownReplay: false,
      /* No shared board: no ranking is read out of its backup RAM yet
       * (arcade/games.mjs); its save stays in the browser. */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: 'モーターレイド',
          blurb: '拳と蹴りで戦うバイクレース。ほかのライダーを殴り、蹴り落とせ。一人でもレースでき、友だちとはルームマッチで：オーナーのバイクがリンクのマスター筐体、ゲストがスレーブ筐体になります。',
          profiles: { motoraid: { name: 'ツイン', about: 'ツイン筐体' },
                      motoraiddx: { name: 'ツイン/DX', about: 'ツイン/DX' } },
          romHint: 'お持ちの <code>motoraid.zip</code>（モーターレイド ツイン）',
          labels: { up: '未使用', down: '未使用', left: '左に傾ける', right: '右に傾ける',
                    b1: 'スロットル', b2: 'ブレーキ', b3: 'パンチ', b4: 'キック' },
        },
      },
    },
  };
  const HOSTS = { play: 'sfight', srally: 'srallyc' };
  /* ?game= names that are another game's set: { game, profile }. */
  const ALIASES = { von: { id: 'vonj', profile: 'von' } };

  /* A game's default profile on this page: its `first` for the language, or
   * its first. */
  function first(id) {
    const g = GAMES[id];
    const want = (g.first || {})[m2hleLang.lang];
    return (g.profiles.find((p) => p.id === want && !p.soon) || g.profiles[0]).id;
  }

  /* { id, profile, entry, region } for this visit, or null for the front page:
   * `entry` is the site profile picked, `profile` the emulator's that runs it,
   * `region` the country it fixes (none: the page's language picks). */
  function pick(loc) {
    const q = new URLSearchParams(loc.search);
    const page = (/\/([a-z0-9_]+)\.html$/.exec(loc.pathname) || [])[1];
    const host = (/^([a-z]+)\./.exec(loc.hostname) || [])[1];
    let id = null;
    const alias = ALIASES[q.get('game')] || (!q.has('game') && ALIASES[page]);
    if (alias) id = alias.id;
    else if (q.has('game')) id = GAMES[q.get('game')] ? q.get('game') : null;
    else if (GAMES[page]) id = page;
    else if (HOSTS[host]) id = HOSTS[host];
    else if ([...q.keys()].some((k) => k !== 'lang')) id = 'sfight';
    if (!id) return null;
    const profiles = GAMES[id].profiles;
    const want = q.get('profile') || (alias && alias.profile);
    const p = profiles.find((e) => e.id === want && !e.soon) || profiles.find((e) => e.id === first(id));
    return { id, profile: p.runs || p.id, entry: p.id, region: p.region || null };
  }

  /* The link that plays a game, relative to the site's root, in this page's
   * language. */
  function href(id, profile) {
    const q = new URLSearchParams({ game: id, profile: profile || first(id) });
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
