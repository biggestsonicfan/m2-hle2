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
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
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
      /* src/profiles/hotd.h: Revision A or the first release (MAME's hotdo), one profile. */
      profiles: [
        { id: 'hotd', name: 'Rev A', about: 'Revision A, or the first release' },
      ],
      romHint: 'Your <code>hotd.zip</code> (The House of the Dead)',
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
          profiles: { hotd: { name: 'リビジョン A', about: 'リビジョン A または初期版' } },
          romHint: 'お持ちの <code>hotd.zip</code>（ザ・ハウス・オブ・ザ・デッド）',
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
      ],
      /* The I/O board's program is MAME's model1io device (src/profiles/daytona.h). */
      romHint: 'Your <code>daytona.zip</code> and the I/O board\'s <code>model1io.zip</code>, picked together',
      /* src/profiles/daytona.h: LEFT/RIGHT the wheel, UP/DOWN the four-speed
       * lever, B1 accelerator, B2 brake, B3/B4 VR1/VR2 (player 2's B3/B4 are
       * VR3/VR4). The wheel's force is the pad's rumble (m2hle-pad.js). */
      labels: { up: 'Shift up', down: 'Shift down', left: 'Steer left', right: 'Steer right',
                b1: 'Accelerator', b2: 'Brake', b3: 'View 1', b4: 'View 2' },
      ownReplay: false,
      /* A shared board (arcade/games.mjs); the save stays in the browser too,
       * even when the arcade cannot be reached (m2hle-page.js). */
      keepSave: true,
      i18n: {
        ja: {
          title: 'デイトナUSA',
          blurb: 'スリーセブン・スピードウェイ、ダイナソー・キャニオン、シーサイド・ストリート・ギャラクシー。ローリングスタート！',
          profiles: { daytona: { name: 'リビジョン A', about: '日本、リビジョン A' } },
          romHint: 'お持ちの <code>daytona.zip</code> と I/O ボードの <code>model1io.zip</code> を一緒に',
          labels: { up: 'シフトアップ', down: 'シフトダウン', left: 'ハンドル左', right: 'ハンドル右',
                    b1: 'アクセル', b2: 'ブレーキ', b3: '視点 1', b4: '視点 2' },
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
      blurb: 'Snow sleds on a linked ring of cabinets. A race needs two boards: open a Room Match, whose owner runs the Main unit (the operator\'s) and whose guest rides the sled.',
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
      /* No shared board or ranking (arcade/games.mjs); its save stays in the browser. */
      arcade: false, keepSave: true,
      i18n: {
        ja: {
          title: 'パワースレッド',
          blurb: '筐体をリンクして走るスノースレッド。レースには基板が2台必要：ルームマッチを開くと、オーナーがメインユニット（オペレーター側）、ゲストがソリに乗ります。',
          profiles: { powsled: { name: 'スレーブ', about: 'ライド、リビジョン A（ルームではメインにも）' },
                      powsledr: { name: 'リレー', about: '大画面（リレーユニット）' } },
          romHint: 'お持ちの <code>powsled.zip</code>（パワースレッド、スレーブユニット）',
          labels: { left: '左ペダル', right: '右ペダル', up: 'メイン：パネルボタン3', down: '未使用',
                    b1: 'エントリー', b2: 'コール', b3: 'ネットワークチェック取消／メイン：レース開始', b4: 'エラー取消' },
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
