/*
 * Gamepads, and the Controls panel that maps them.
 *
 * The browser's Gamepad API has no events for buttons, only a snapshot to poll,
 * so this polls it once per display frame and hands the emulator one bit per
 * GAME_INPUT_* action (main_web.c, web_pad_set). The emulator presses and
 * releases only what changed, so a pad and the keyboard can hold the same
 * direction without letting go of each other.
 *
 * The mapping is kept in this origin's localStorage under 'm2hle.pad' and applies
 * to every pad. The first pad plays player 1; a second plays player 2 unless the
 * panel says otherwise. Under netplay both reach the local player (netplay.h,
 * netplay_sample_local), which is what a player with two pads plugged in expects.
 * A four-player game (Air Walkers, web_four_players) gives a third and fourth
 * pad to players 3 and 4 (web_pad_far_set), on this machine only: the board
 * reads them only outside a session.
 *
 * Browsers hide pads from a page until a button is pressed on one: "no controller"
 * before that is not a fault.
 */
'use strict';

/* The words (m2hle-lang.js). The rows' names are m2hle-keys.js's keys.act.*. */
m2hleLang.add({
  en: {
    'pad.std.0': 'A / Cross', 'pad.std.1': 'B / Circle', 'pad.std.2': 'X / Square', 'pad.std.3': 'Y / Triangle',
    'pad.std.4': 'Left bumper', 'pad.std.5': 'Right bumper', 'pad.std.6': 'Left trigger', 'pad.std.7': 'Right trigger',
    'pad.std.8': 'Back / Select', 'pad.std.9': 'Start', 'pad.std.10': 'Left stick press',
    'pad.std.11': 'Right stick press', 'pad.std.12': 'D-pad up', 'pad.std.13': 'D-pad down',
    'pad.std.14': 'D-pad left', 'pad.std.15': 'D-pad right', 'pad.std.16': 'Home',
    'pad.axis.0-': 'Left stick left', 'pad.axis.0+': 'Left stick right',
    'pad.axis.1-': 'Left stick up', 'pad.axis.1+': 'Left stick down',
    'pad.axis.2-': 'Right stick left', 'pad.axis.2+': 'Right stick right',
    'pad.axis.3-': 'Right stick up', 'pad.axis.3+': 'Right stick down',
    'pad.button': 'Button {n}',
    'pad.axis': 'Axis {n} {dir}',
    'pad.sub.macros': 'Macros: one press holds several buttons',
    'pad.sub.page': 'The page: offline only, as the button beside the menu',
    'pad.sub.cab': 'The cabinet: the first pad only',
    'pad.none': 'Not set',
    'pad.remove': 'Remove',
    'pad.remove.from': 'Remove {button} from {action}',
    'pad.add': 'Add',
    'pad.press': 'Press a button…',
    'pad.status.none': 'No controller found. Connect one and press any button on it.',
    'pad.status.p1': 'Player 1: {name}',
    'pad.status.p2': 'Player 2: {name}',
    'pad.status.p3': 'Player 3: {name}', 'pad.status.p4': 'Player 4: {name}',
    'pad.status.also': 'Also player 1: {name}',
    'pad.status.odd': '{pads} (unrecognised layout: buttons are shown by number)',
    'pad.status.twin': '{pads} (Saturn Twin Stick layout, through the Bliss-Box)',
  },
  ja: {
    'pad.std.0': 'A / ×', 'pad.std.1': 'B / ○', 'pad.std.2': 'X / □', 'pad.std.3': 'Y / △',
    'pad.std.4': '左バンパー', 'pad.std.5': '右バンパー', 'pad.std.6': '左トリガー', 'pad.std.7': '右トリガー',
    'pad.std.8': 'Back / セレクト', 'pad.std.9': 'スタート', 'pad.std.10': '左スティック押し込み',
    'pad.std.11': '右スティック押し込み', 'pad.std.12': '十字キー上', 'pad.std.13': '十字キー下',
    'pad.std.14': '十字キー左', 'pad.std.15': '十字キー右', 'pad.std.16': 'ホーム',
    'pad.axis.0-': '左スティック左', 'pad.axis.0+': '左スティック右',
    'pad.axis.1-': '左スティック上', 'pad.axis.1+': '左スティック下',
    'pad.axis.2-': '右スティック左', 'pad.axis.2+': '右スティック右',
    'pad.axis.3-': '右スティック上', 'pad.axis.3+': '右スティック下',
    'pad.button': 'ボタン{n}',
    'pad.axis': '軸{n} {dir}',
    'pad.sub.macros': 'マクロ：1回押すと複数のボタンを同時に押します',
    'pad.sub.page': 'ページ：オフライン時のみ。メニュー横のボタンと同じです',
    'pad.sub.cab': '筐体：1台目のパッドのみ',
    'pad.none': '未設定',
    'pad.remove': '削除',
    'pad.remove.from': '{action}から{button}を削除',
    'pad.add': '追加',
    'pad.press': 'ボタンを押してください…',
    'pad.status.none': 'コントローラーが見つかりません。接続して、いずれかのボタンを押してください。',
    'pad.status.p1': 'プレイヤー1：{name}',
    'pad.status.p2': 'プレイヤー2：{name}',
    'pad.status.p3': 'プレイヤー3：{name}', 'pad.status.p4': 'プレイヤー4：{name}',
    'pad.status.also': 'こちらもプレイヤー1：{name}',
    'pad.status.odd': '{pads}（未対応の配置のため、ボタンは番号で表示しています）',
    'pad.status.twin': '{pads}（Bliss-Box 経由のセガサターン ツインスティック配置）',
  },
});

const m2hlePad = (() => {
  const $ = (id) => document.getElementById(id);

  /* A twin-stick game (Virtual-On, m2hle-games.js `twin`) is one cabinet with two
   * levers, and the right lever is player 2's port (src/profiles/vonj.h). One pad
   * then plays both levers: the right lever and its trigger and dash are rows of
   * their own, a second pad plays the same cabinet, and there are no macros. Its
   * bindings are kept apart from the fighting games' ('m2hle.pad.vonj'), which
   * mean something else by every button. */
  const TWIN = !!m2hleGame.twin;
  const STORE = TWIN ? 'm2hle.pad.' + m2hleGame.id : 'm2hle.pad';
  /* A light-gun game's flash on each shot (lightgun_host.h), off unless asked for. */
  const GUN = !!m2hleGame.gun;
  const GUN_FLASH_KEY = 'm2hle.gunflash';
  const GUN_FLASH_FRAMES = 2;
  let gunFlash = false;
  try { gunFlash = localStorage.getItem(GUN_FLASH_KEY) === '1'; } catch (e) { /* private mode */ }

  /* The rows of the panel. `act` is the GAME_INPUT_* index for player 1
   * (src/core/game_profile.h), `acts` a macro's; player 2's is 10 further on. */
  const ALL = [
    { id: 'up',    act: 0 },
    { id: 'down',  act: 1 },
    { id: 'left',  act: 2 },
    { id: 'right', act: 3 },
    /* A twin-stick game's right lever: player 2's directions, B1 and B2. */
    { id: 'rup',    act: 10, twin: true },
    { id: 'rdown',  act: 11, twin: true },
    { id: 'rleft',  act: 12, twin: true },
    { id: 'rright', act: 13, twin: true },
    { id: 'b1',    act: 4 },
    { id: 'b2',    act: 5 },
    { id: 'rshot', act: 14, twin: true },
    { id: 'rdash', act: 15, twin: true },
    { id: 'b3',    act: 6 },
    { id: 'b4',    act: 7 },
    { id: 'start', act: 8 },
    { id: 'coin',  act: 9 },
    /* Macros: one binding that holds several buttons at once, as the Gems
     * Collection and HD ports offer. Unbound until the player binds them. */
    { id: 'pk',    label: 'P + K',     acts: [4, 5],    macro: true },
    { id: 'pb',    label: 'P + B',     acts: [4, 6],    macro: true },
    { id: 'kb',    label: 'K + B',     acts: [5, 6],    macro: true },
    { id: 'pkb',   label: 'P + K + B', acts: [4, 5, 6], macro: true },
    /* The cabinet's own switches, the first pad only: the board's Service and
     * Test, and a racing game's gear buttons (m2hleGame.gears), one per gate of
     * the lever (src/board/io_5649.h). Unbound until the player binds them. */
    { id: 'service', act: 20, cab: true },
    { id: 'test',    act: 21, cab: true },
    { id: 'gearn',   act: 22, cab: true, gears: true },
    { id: 'gear1',   act: 23, cab: true, gears: true },
    { id: 'gear2',   act: 24, cab: true, gears: true },
    { id: 'gear3',   act: 25, cab: true, gears: true },
    { id: 'gear4',   act: 26, cab: true, gears: true },
    /* The page's Pause (m2hle-page.js), not a game button: it holds nothing.
     * Unbound until the player binds it, as every standard button has a job. */
    { id: 'pause', acts: [],        page: true },
  ];
  const ACTIONS = ALL.filter((a) => (TWIN ? !a.macro : !a.twin) && (!a.gears || m2hleGame.gears));
  for (const a of ACTIONS) a.mask = (a.acts || [a.act]).reduce((m, i) => m | (1 << i), 0);
  for (const a of ACTIONS) a.label = m2hleGame.labels[a.id] || a.label || m2hleLang.t('keys.act.' + a.id);
  const P2_OFFSET = 10;

  /* A binding is a button ({b: index}) or one direction of an axis ({a: index, d: ±1}).
   * The defaults are for the "standard" layout, which is what the browser reports for
   * most XInput, PlayStation and Switch pads. */
  const DEFAULTS = {
    up:    [{ b: 12 }, { a: 1, d: -1 }],
    down:  [{ b: 13 }, { a: 1, d: 1 }],
    left:  [{ b: 14 }, { a: 0, d: -1 }],
    right: [{ b: 15 }, { a: 0, d: 1 }],
    b1:    [{ b: 2 }],
    b2:    [{ b: 0 }],
    b3:    [{ b: 1 }],
    b4:    [{ b: 3 }],
    start: [{ b: 9 }],
    coin:  [{ b: 8 }],
    pk: [], pb: [], kb: [], pkb: [], pause: [],
    service: [], test: [], gearn: [], gear1: [], gear2: [], gear3: [], gear4: [],
  };

  /* A twin-stick game on a standard pad, as YAMP's full set does it: the d-pad and
   * left stick are the left lever, the face buttons (where they sit, Y up, A down)
   * and the right stick the right lever, the triggers shoot and the bumpers dash. */
  const TWIN_DEFAULTS = {
    up:     [{ b: 12 }, { a: 1, d: -1 }],
    down:   [{ b: 13 }, { a: 1, d: 1 }],
    left:   [{ b: 14 }, { a: 0, d: -1 }],
    right:  [{ b: 15 }, { a: 0, d: 1 }],
    rup:    [{ b: 3 }, { a: 3, d: -1 }],
    rdown:  [{ b: 0 }, { a: 3, d: 1 }],
    rleft:  [{ b: 2 }, { a: 2, d: -1 }],
    rright: [{ b: 1 }, { a: 2, d: 1 }],
    b1:     [{ b: 6 }],
    b2:     [{ b: 4 }],
    rshot:  [{ b: 7 }],
    rdash:  [{ b: 5 }],
    b3: [], b4: [],
    start:  [{ b: 9 }],
    coin:   [{ b: 8 }],
    pause: [], service: [], test: [],
  };

  /* Sega's Saturn Twin Stick (HSS-0151) through a Bliss-Box adapter, which the
   * browser reports with no standard layout: HID button n is buttons[n - 1]. The
   * stick is a Saturn pad inside, so the left lever is the d-pad, which the adapter
   * puts on the X / Y axes (y low = up), and the right lever is four face buttons:
   * up Y (HID 4), down B (2), left X (3), right Z (7). The right trigger is A (1),
   * its thumb button C (8), the left trigger L (9), its thumb R (10), Start 6.
   * Measured on a stick by YAMP (rokuYAMP docs/von-twin-stick.md); the adapter
   * cannot tell the Twin Stick from a Saturn pad, which this maps the same way.
   * Its hat is left alone: one firmware rests it at 0, which reads as up. */
  const BLISSBOX_DEFAULTS = {
    up:     [{ a: 1, d: -1 }],
    down:   [{ a: 1, d: 1 }],
    left:   [{ a: 0, d: -1 }],
    right:  [{ a: 0, d: 1 }],
    rup:    [{ b: 3 }],
    rdown:  [{ b: 1 }],
    rleft:  [{ b: 2 }],
    rright: [{ b: 6 }],
    b1:     [{ b: 8 }],
    b2:     [{ b: 9 }],
    rshot:  [{ b: 0 }],
    rdash:  [{ b: 7 }],
    b3: [], b4: [],
    start:  [{ b: 5 }],
    coin: [], pause: [], service: [], test: [],
  };

  /* Bliss-Box: vendor 16d0, product 0d04 and up, one per port. Chrome names a pad
   * "... (Vendor: 16d0 Product: 0d04)", Firefox "16d0-0d04-...". A layout the browser
   * already maps to "standard" is a standard pad. */
  const isBlissBox = (pad) => pad.mapping !== 'standard' &&
    (/bliss-?box/i.test(pad.id) || /\b16d0\b.{0,12}\b0d0[0-9a-f]\b/i.test(pad.id));

  /* Which set of bindings plays a pad: a twin-stick game keeps one for the Twin
   * Stick and one for every other pad. */
  const layoutOf = (pad) => (TWIN && pad && isBlissBox(pad) ? 'blissbox' : 'std');
  const defaultSets = () => (TWIN ? { std: clone(TWIN_DEFAULTS), blissbox: clone(BLISSBOX_DEFAULTS) }
                                  : { std: clone(DEFAULTS) });
  const MAX_BINDS = 4;
  const PRESS = 0.5;          /* a button counts as held past this (analogue triggers) */
  const DEAD = 0.5;           /* an axis counts as held past this */
  const GRAB = 0.6;           /* ...and must move this far from rest to be captured */

  const clone = (o) => JSON.parse(JSON.stringify(o));
  let sets = defaultSets();   /* bindings per layout (layoutOf) */
  let secondIsP2 = true;
  let ready = false;          /* the emulator's exports exist */
  let listening = null;       /* {id, rest: {buttons, axes} per pad index} while capturing */
  let padMask = 0;            /* what the pads held at the last poll */
  let touchMask = 0;          /* what the touch buttons hold (m2hle-touch.js, setTouch) */
  let keyMask = 0;            /* what the keyboard holds (m2hle-keys.js, setKeys) */
  let pauseHeld = false;      /* a pad held Pause at the last poll */
  let claim = null;           /* shown the pads each poll; true takes them from the game (setClaim) */
  const PAUSE_ROW = 1 << ACTIONS.findIndex((a) => a.id === 'pause');
  /* The cabinet rows' action bits: only the first pad presses them. */
  const CAB_BITS = ACTIONS.reduce((m, a) => (a.cab ? m | a.mask : m), 0);

  function load() {
    try {
      const s = JSON.parse(localStorage.getItem(STORE) || 'null');
      if (!s || s.v !== 1) return;
      loadSet(sets.std, s.binds);
      if (TWIN) loadSet(sets.blissbox, s.blissbox);
      if (typeof s.secondIsP2 === 'boolean') secondIsP2 = s.secondIsP2;
    } catch (e) { /* private mode, or a value from somewhere else: keep the defaults */ }
  }

  function loadSet(binds, saved) {
    for (const a of ACTIONS) {
      const list = saved && saved[a.id];
      if (Array.isArray(list)) binds[a.id] = list.filter(valid).slice(0, MAX_BINDS);
    }
  }

  function save() {
    const s = TWIN ? { v: 1, binds: sets.std, blissbox: sets.blissbox } : { v: 1, binds: sets.std, secondIsP2 };
    try { localStorage.setItem(STORE, JSON.stringify(s)); } catch (e) { /* private mode */ }
  }

  function valid(x) {
    return x && ((Number.isInteger(x.b) && x.b >= 0 && x.b < 64) ||
                 (Number.isInteger(x.a) && x.a >= 0 && x.a < 16 && (x.d === 1 || x.d === -1)));
  }

  const same = (x, y) => x.b === y.b && x.a === y.a && x.d === y.d;

  /* ---- Names ------------------------------------------------------------------ */

  /* The "standard" layout's buttons and stick axes have names (pad.std.*, pad.axis.*). */
  const STD_BUTTONS = 17, STD_AXES = 4;

  /* A Bliss-Box's buttons as the Saturn pad (or Twin Stick) on it names them, by
   * the vendor's HID numbering (BLISSBOX_DEFAULTS); its X / Y axes are the d-pad. */
  const SATURN = ['A', 'B', 'X', 'Y', '', 'Start', 'Z', 'C', 'L', 'R'];

  function saturnName(x) {
    if (x.b !== undefined) return SATURN[x.b] ? 'Saturn ' + SATURN[x.b] : null;
    if (x.a > 1) return null;
    const dpad = x.a === 1 ? (x.d < 0 ? 12 : 13) : (x.d < 0 ? 14 : 15);
    return m2hleLang.t('pad.std.' + dpad);
  }

  function bindName(x, standard, layout) {
    const t = m2hleLang.t;
    const saturn = layout === 'blissbox' && saturnName(x);
    if (saturn) return saturn;
    if (x.b !== undefined) return standard && x.b < STD_BUTTONS ? t('pad.std.' + x.b) : t('pad.button', { n: x.b });
    if (standard && x.a < STD_AXES) return t('pad.axis.' + x.a + (x.d > 0 ? '+' : '-'));
    return t('pad.axis', { n: x.a, dir: x.d > 0 ? '+' : '−' });
  }

  /* ---- Polling ---------------------------------------------------------------- */

  function pads() {
    const out = [];
    const list = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const p of list) if (p && p.connected) out.push(p);
    return out;
  }

  function held(pad, x) {
    if (x.b !== undefined) {
      const btn = pad.buttons[x.b];
      return !!btn && (btn.pressed || btn.value > PRESS);
    }
    const v = pad.axes[x.a];
    return v !== undefined && v * x.d > DEAD;
  }

  /* Does the pad at this index play player 3 or 4 (a four-player game's third
   * and fourth pads)? */
  function farPad(i) {
    return (i === 2 || i === 3) && !TWIN && ready && !!Module._web_four_players && Module._web_four_players() !== 0;
  }

  /* The rows this pad holds, one bit per ACTIONS index. */
  function padRows(pad) {
    let r = 0;
    const binds = sets[layoutOf(pad)];
    ACTIONS.forEach((a, i) => { if (binds[a.id].some((x) => held(pad, x))) r |= 1 << i; });
    return r;
  }

  /* One bit per player-1 action (0..9) those rows hold. */
  function rowActions(r) {
    let m = 0;
    ACTIONS.forEach((a, i) => { if (r & (1 << i)) m |= a.mask; });
    return m;
  }

  /* What the pads hold: the near players' actions, the far players' (a
   * four-player cabinet's 3P / 4P), and whether any asks for the pause. */
  function readPads(list) {
    let mask = 0, far = 0, pause = false;
    list.forEach((pad, i) => {
      const r = padRows(pad), m = i === 0 ? rowActions(r) : rowActions(r) & ~CAB_BITS;
      if (farPad(i)) far |= i === 3 ? m << P2_OFFSET : m;
      else mask |= (i === 1 && secondIsP2 && !TWIN) ? m << P2_OFFSET : m;
      pause = pause || !!(r & PAUSE_ROW);
      if (i === 0) showHeld(r);
    });
    return { mask, far, pause };
  }

  /* The far players' actions to the board. */
  function sendFar(far) {
    if (ready && Module._web_pad_far_set) Module._web_pad_far_set(far >>> 0);
  }

  /* Something else has taken the pads (claim): the board holds nothing of theirs. */
  function letGo() {
    padMask = 0; pauseHeld = false; showHeld(0);
    sendFar(0);
    send();
  }

  function poll() {
    requestAnimationFrame(poll);
    const list = pads();
    if (listening) { capture(list); return; }
    if (claim && claim(list)) { letGo(); return; }

    let { mask, far, pause } = readPads(list);
    if (pause && !pauseHeld) window.dispatchEvent(new Event('m2hle-pause'));
    pauseHeld = pause;
    if (list.length === 0) showHeld(0);
    if (ready && Module._web_wheel) mask |= wheel(list);
    padMask = mask;
    if (!listening) sendFar(far);
    /* Every frame, not only on a change: the emulator forgets what the pad held
     * whenever it lets go of the keyboard (focus lost, the drawer opened), and a
     * direction still held should come straight back. */
    send();
    if (ready && Module._web_ffb) rumble(list);
  }

  /* ---- Analog wheel and pedals -------------------------------------------------
   *
   * Sega Rally and Daytona USA read where the wheel is turned and how far a
   * pedal is down (main_web.c, web_wheel; Pinboard #1082). Player 1's pads
   * give them: the axis bound to Left / Right turns the wheel (a "standard"
   * pad's stick past a small dead zone, any other pad's axis, a racing wheel's,
   * all of it), and the analog value of what is bound to Button 1 (the
   * accelerator) and Button 2 (the brake) presses the pedals; a standard
   * pad's right and left triggers are those pedals too. A pedal on an axis is
   * read from where it rested when first seen. Sessions and replays read the
   * buttons, so the triggers press Button 1 / 2 past half as well. */
  const STICK_DEAD = 2500 / 32767, PEDAL_HALF = 0.5;
  const wheelRest = new Map();   /* pad index -> the axes as first seen */

  function axisPedal(pad, x) {
    let rest = wheelRest.get(pad.index);
    if (!rest) { rest = pad.axes.slice(); wheelRest.set(pad.index, rest); }
    const r = rest[x.a] || 0, v = pad.axes[x.a] || 0;
    const span = x.d > 0 ? 1 - r : r + 1;
    return span > 0.25 ? Math.min(1, Math.max(0, (v - r) * x.d / span)) : 0;
  }

  /* Where a pad turns the wheel, -1 .. 1: the furthest of its Left / Right axes. */
  function padSteer(pad, binds, std) {
    let st = 0;
    for (const x of binds.left || []) if (x.a !== undefined) { const v = -(pad.axes[x.a] || 0) * x.d; if (Math.abs(v) > Math.abs(st)) st = v; }
    for (const x of binds.right || []) if (x.a !== undefined) { const v = (pad.axes[x.a] || 0) * x.d; if (Math.abs(v) > Math.abs(st)) st = v; }
    if (std) st = Math.abs(st) <= STICK_DEAD ? 0 : Math.sign(st) * (Math.abs(st) - STICK_DEAD) / (1 - STICK_DEAD);
    return st;
  }

  /* How far a pad presses a pedal, 0 .. 1: what row is bound to, or a standard
   * pad's trigger. */
  function padPedal(pad, binds, std, row, trigger) {
    let p = std && pad.buttons[trigger] ? pad.buttons[trigger].value : 0;
    for (const x of binds[row] || []) {
      const v = x.b !== undefined ? (pad.buttons[x.b] ? pad.buttons[x.b].value : 0) : axisPedal(pad, x);
      if (v > p) p = v;
    }
    return p;
  }

  function wheel(list) {
    if (!Module._web_wheel_on || !Module._web_wheel_on()) { Module._web_wheel(0, 0, 0, 0); return 0; }
    let on = false, steer = 0, accel = 0, brake = 0, mask = 0;
    list.forEach((pad, i) => {
      if (i === 1 && secondIsP2 && !TWIN) return;
      const binds = sets[layoutOf(pad)], std = pad.mapping === 'standard';
      const st = padSteer(pad, binds, std);
      const ac = padPedal(pad, binds, std, 'b1', 7), br = padPedal(pad, binds, std, 'b2', 6);
      if (Math.abs(st) > Math.abs(steer)) steer = st;
      accel = Math.max(accel, ac);
      brake = Math.max(brake, br);
      if (std && pad.buttons[7] && pad.buttons[7].value > PEDAL_HALF) mask |= 1 << 4;   /* GAME_INPUT_P1_B1 */
      if (std && pad.buttons[6] && pad.buttons[6].value > PEDAL_HALF) mask |= 1 << 5;   /* GAME_INPUT_P1_B2 */
      on = true;
    });
    const q = (v) => Math.round(Math.max(-1, Math.min(1, v)) * 32767);
    Module._web_wheel(on ? 1 : 0, q(steer), q(accel), q(brake));
    return mask;
  }

  /* ---- Force feedback ----------------------------------------------------------
   *
   * The drive board (main_web.c, web_ffb): Daytona USA's kicks and shakes and
   * Sega Rally's hard turns of the wheel (a slide, a crash) on player 1's pad,
   * the two motors of a 'dual-rumble' effect (Chrome, Edge, Safari) or a pulse
   * (Firefox), renewed while they last. With no pad, a phone buzzes for each
   * kick the board starts, at most once a RUMBLE_MS. A pad has no spring or
   * friction, so the wheel's weight is not in it. */
  const RUMBLE_MS = 200, RUMBLE_RENEW_MS = 120;
  let rumbleOn = false, rumbleAt = 0, rumbleCmd = 0;

  function padRumble(pad, strong, weak) {
    const fx = pad.vibrationActuator;
    try {
      if (fx && fx.playEffect) {
        if (strong || weak) fx.playEffect('dual-rumble', { startDelay: 0, duration: RUMBLE_MS, strongMagnitude: strong, weakMagnitude: weak });
        else if (fx.reset) fx.reset();
      } else if (pad.hapticActuators && pad.hapticActuators[0]) {
        pad.hapticActuators[0].pulse(Math.max(strong, weak), strong || weak ? RUMBLE_MS : 0);
      }
    } catch (e) { /* a pad that cannot rumble */ }
  }

  function rumble(list) {
    const v = Module._web_ffb() >>> 0;
    const strong = ((v >>> 16) & 255) / 255, weak = ((v >>> 8) & 255) / 255, cmd = v & 255;
    const on = strong > 0 || weak > 0, now = performance.now();
    if (list.length) {
      if (on && (!rumbleOn || cmd !== rumbleCmd || now - rumbleAt >= RUMBLE_RENEW_MS)) {
        padRumble(list[0], strong, weak);
        rumbleAt = now;
      } else if (!on && rumbleOn) padRumble(list[0], 0, 0);
    } else if (on && cmd !== rumbleCmd && now - rumbleAt >= RUMBLE_MS && touchMask && navigator.vibrate) {
      navigator.vibrate(Math.round(40 + 80 * Math.max(strong, weak)));
      rumbleAt = now;
    }
    rumbleOn = on;
    rumbleCmd = cmd;
  }

  /* The touch buttons and the keyboard share this one channel to the emulator:
   * web_pad_set takes the whole mask, so two callers would let go of each other's
   * presses. */
  function send() {
    if (ready && !listening) Module._web_pad_set((padMask | touchMask | keyMask) >>> 0);
  }

  /* A key goes out now rather than at the next poll, for the same reason. */
  function setKeys(m) {
    keyMask = m >>> 0;
    send();
  }

  /* A touch press goes out now rather than at the next poll: a tap is short. */
  function setTouch(m) {
    touchMask = m >>> 0;
    send();
  }

  /* ---- The panel -------------------------------------------------------------- */

  function snapshot(list) {
    const rest = new Map();
    for (const p of list) {
      rest.set(p.index, { buttons: p.buttons.map((b) => b.pressed || b.value > PRESS), axes: p.axes.slice() });
    }
    return rest;
  }

  function listen(id) {
    if (listening && listening.id === id) { stopListening(); return; }
    listening = { id, rest: snapshot(pads()) };
    /* Nothing reaches the game while a press is being captured: binding Punch
     * should not throw one. */
    if (ready) Module._web_pad_set(0);
    if (ready && Module._web_pad_far_set) Module._web_pad_far_set(0);
    render();
  }

  function stopListening() {
    listening = null;
    render();
  }

  /* The first thing pressed or pushed since listen() began. A pad plugged in
   * mid-capture gets its rest state from its first poll. */
  function capture(list) {
    for (const p of list) {
      let rest = listening.rest.get(p.index);
      if (!rest) { listening.rest.set(p.index, snapshot([p]).get(p.index)); continue; }
      let got = null;
      p.buttons.forEach((b, i) => {
        const down = b.pressed || b.value > PRESS;
        if (down && !rest.buttons[i] && !got) got = { b: i };
        rest.buttons[i] = down;
      });
      if (!got) {
        p.axes.forEach((v, i) => {
          if (!got && Math.abs(v) > GRAB && Math.abs(v - (rest.axes[i] || 0)) > GRAB) got = { a: i, d: v > 0 ? 1 : -1 };
        });
      }
      if (got) {
        /* It binds on the layout of the pad that pressed it. */
        const binds = sets[layoutOf(p)];
        /* A button is the game's or Pause's, not both: binding it to one takes it
         * off the other, so Select can move from Coin to Pause in one step. */
        for (const a of ACTIONS) {
          if (!!a.page !== (listening.id === 'pause')) binds[a.id] = binds[a.id].filter((x) => !same(x, got));
        }
        pauseHeld = true;   /* the press that bound it does not pause */
        const list2 = binds[listening.id];
        if (!list2.some((x) => same(x, got))) {
          if (list2.length >= MAX_BINDS) list2.shift();
          list2.push(got);
          save();
        }
        stopListening();
        return;
      }
    }
  }

  function render() {
    const rows = $('pad-rows');
    if (!rows) return;
    const list = pads();
    const standard = list.length === 0 || list[0].mapping === 'standard';
    /* The panel shows and edits the first pad's layout. */
    const layout = layoutOf(list[0]);
    const binds = sets[layout];
    rows.textContent = '';
    for (const a of ACTIONS) {
      if (a.macro && a === ACTIONS.find((x) => x.macro)) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = m2hleLang.t('pad.sub.macros');
        rows.appendChild(sub);
      }
      if (a.cab && a === ACTIONS.find((x) => x.cab)) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = m2hleLang.t('pad.sub.cab');
        rows.appendChild(sub);
      }
      if (a.page) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = m2hleLang.t('pad.sub.page');
        rows.appendChild(sub);
      }
      const tr = document.createElement('li');
      tr.className = 'pad-row';
      tr.id = 'pad-row-' + a.id;
      const name = document.createElement('span');
      name.className = 'pad-name';
      name.textContent = a.label;
      tr.appendChild(name);

      const chips = document.createElement('span');
      chips.className = 'pad-chips';
      if (binds[a.id].length === 0) {
        const none = document.createElement('span');
        none.className = 'status';
        none.textContent = m2hleLang.t('pad.none');
        chips.appendChild(none);
      }
      binds[a.id].forEach((x, i) => {
        const chip = document.createElement('button');
        chip.className = 'pad-chip';
        chip.type = 'button';
        chip.title = m2hleLang.t('pad.remove');
        chip.setAttribute('aria-label', m2hleLang.t('pad.remove.from', { button: bindName(x, standard, layout), action: a.label }));
        /* the pad's own faces (m2hle-prompts.js); a Bliss-Box's Saturn names stay words */
        const icon = standard && layout === 'std' && m2hlePrompts.pad(x, list[0]);
        m2hlePrompts.chip(chip, icon, bindName(x, standard, layout));
        chip.addEventListener('click', () => { binds[a.id].splice(i, 1); save(); render(); });
        chips.appendChild(chip);
      });
      tr.appendChild(chips);

      const add = document.createElement('button');
      add.className = 'tool pad-add';
      add.type = 'button';
      const on = listening && listening.id === a.id;
      add.textContent = m2hleLang.t(on ? 'pad.press' : 'pad.add');
      add.setAttribute('aria-pressed', String(!!on));
      add.addEventListener('click', () => listen(a.id));
      tr.appendChild(add);
      rows.appendChild(tr);
    }
    $('pad-p2').checked = secondIsP2;
    $('pad-p2').parentElement.hidden = TWIN;
    status(list);
  }

  function status(list) {
    const el = $('pad-status');
    if (!el) return;
    if (list.length === 0) {
      el.textContent = m2hleLang.t('pad.status.none');
      return;
    }
    const names = list.map((p, i) => m2hleLang.t(i === 0 ? 'pad.status.p1' : (i === 1 && secondIsP2 && !TWIN ? 'pad.status.p2'
                                                 : farPad(i) ? (i === 2 ? 'pad.status.p3' : 'pad.status.p4') : 'pad.status.also'),
                                                 { name: p.id.replace(/\s*\(.*?Vendor.*?\)\s*/i, ' ').trim() }));
    const pads = names.join(' · ');
    el.textContent = list[0].mapping === 'standard' ? pads
      : m2hleLang.t(layoutOf(list[0]) === 'blissbox' ? 'pad.status.twin' : 'pad.status.odd', { pads });
  }

  /* The rows light up while the first pad holds them: the quickest way to check a mapping. */
  let shownHeld = -1;
  function showHeld(r) {
    if (r === shownHeld || $('controls').hidden) return;
    shownHeld = r;
    ACTIONS.forEach((a, i) => {
      const row = $('pad-row-' + a.id);
      if (row) row.classList.toggle('pad-held', !!(r & (1 << i)));
    });
  }

  /* The panel's tabs: Keyboard, Controller, Touch. Until one is picked it opens
   * on Keyboard, or on Touch on a phone (the touch buttons are what it is for there). */
  const TABS = { keys: 'key-section', pad: 'pad-section', touch: 'touch-section' };
  let tabPicked = null;

  function showTab(tab) {
    if (tab !== 'pad') stopListening();
    for (const t in TABS) {
      $('ctl-tab-' + t).setAttribute('aria-selected', String(t === tab));
      $(TABS[t]).hidden = t !== tab;
    }
  }

  function toggle(open) {
    const panel = $('controls');
    if (open === undefined) open = panel.hidden;
    if (!open) stopListening();
    /* Play online opens in the same place: one at a time. */
    if (open && !$('online').hidden) $('np-close').click();
    if (open) showTab(tabPicked || (matchMedia('(pointer: coarse)').matches || !$('touch').hidden ? 'touch' : 'keys'));
    panel.hidden = !open;
    $('btn-controls').setAttribute('aria-expanded', String(open));
    shownHeld = -1;
    if (open) render();
    else $('canvas').focus();
  }

  function init() {
    load();
    $('btn-controls').addEventListener('click', () => toggle());
    $('pad-close').addEventListener('click', () => toggle(false));
    for (const t in TABS) $('ctl-tab-' + t).addEventListener('click', () => { tabPicked = t; showTab(t); });
    $('btn-online').addEventListener('click', () => toggle(false));
    $('pad-reset').addEventListener('click', () => { sets = defaultSets(); secondIsP2 = true; save(); stopListening(); });
    $('pad-p2').addEventListener('change', (e) => { secondIsP2 = e.target.checked; save(); render(); });
    $('gun-flash-row').hidden = !GUN;
    $('gun-flash').checked = gunFlash;
    $('gun-flash').addEventListener('change', (e) => {
      gunFlash = e.target.checked;
      try { localStorage.setItem(GUN_FLASH_KEY, gunFlash ? '1' : '0'); } catch (err) { /* private mode */ }
      if (ready) Module._web_set_gun_flash(gunFlash ? GUN_FLASH_FRAMES : 0);
    });
    $('controls').addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { if (listening) stopListening(); else toggle(false); }
    });
    window.addEventListener('gamepadconnected', (e) => {
      m2hleTools.add('gamepad: connected "' + e.gamepad.id + '" (' + (e.gamepad.mapping || 'no standard layout') + ')');
      if (!$('controls').hidden) render();
    });
    window.addEventListener('gamepaddisconnected', (e) => {
      m2hleTools.add('gamepad: disconnected "' + e.gamepad.id + '"');
      if (!$('controls').hidden) render();
    });
    requestAnimationFrame(poll);
  }

  /* Called from Module.onM2hleReady. */
  /* A page menu the pads drive (m2hle-cardboard.js): fn(pads) sees them every
   * poll, and while it returns true the game gets nothing from them. */
  function setClaim(fn) { claim = fn; }

  function onReady() {
    ready = true;
    if (GUN) Module._web_set_gun_flash(gunFlash ? GUN_FLASH_FRAMES : 0);
  }

  document.addEventListener('DOMContentLoaded', init);

  return { onReady, toggle, setTouch, setKeys, setClaim, get binds() { return clone(sets.std); }, get sets() { return clone(sets); } };
})();
