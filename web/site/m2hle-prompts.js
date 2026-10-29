/*
 * Input prompt icons for the Controls panel's chips: Kenney's Input Prompts
 * (www.kenney.nl, CC0; prompts/License.txt), the SVGs in prompts/ that
 * tools/prompts/import_kenney.py copies. The PS3-style menus of the libretro core
 * draw the same pack (src/ui/input_prompts.h).
 *
 * A pad's buttons are shown in its own family's faces, told from the Gamepad
 * API's id (Chrome: "... Vendor: 054c Product: 0ce6)", Firefox: "054c-0ce6-...").
 * Only a standard-mapping pad gets icons: on any other the indices are the
 * driver's and the chips keep their text.
 */
'use strict';

const m2hlePrompts = (() => {
  const DIRS = ['up', 'down', 'left', 'right'];

  /* Standard-mapping buttons 0..16, per family. Nintendo's standard index 0 is
   * the bottom face, which Nintendo labels B. */
  const PS = (select, start) => [
    'playstation_button_color_cross', 'playstation_button_color_circle',
    'playstation_button_color_square', 'playstation_button_color_triangle',
    'playstation_trigger_l1', 'playstation_trigger_r1', 'playstation_trigger_l2', 'playstation_trigger_r2',
    select, start, 'playstation_button_l3', 'playstation_button_r3',
    ...DIRS.map((d) => 'playstation_dpad_' + d), null,
  ];
  const BUTTONS = {
    ps3: PS('playstation3_button_select', 'playstation3_button_start'),
    ps4: PS('playstation4_button_share', 'playstation4_button_options'),
    ps5: PS('playstation5_button_create', 'playstation5_button_options'),
    xbox: [
      'xbox_button_color_a', 'xbox_button_color_b', 'xbox_button_color_x', 'xbox_button_color_y',
      'xbox_lb', 'xbox_rb', 'xbox_lt', 'xbox_rt', 'xbox_button_view', 'xbox_button_menu', 'xbox_ls', 'xbox_rs',
      ...DIRS.map((d) => 'xbox_dpad_' + d), 'xbox_guide',
    ],
    switch: [
      'switch_button_b', 'switch_button_a', 'switch_button_y', 'switch_button_x',
      'switch_button_l', 'switch_button_r', 'switch_button_zl', 'switch_button_zr',
      'switch_button_minus', 'switch_button_plus', 'switch_stick_l_press', 'switch_stick_r_press',
      ...DIRS.map((d) => 'switch_dpad_' + d), 'switch_button_home',
    ],
  };
  const STICK = { ps3: 'playstation', ps4: 'playstation', ps5: 'playstation', xbox: 'xbox', switch: 'switch' };

  /* Which faces a pad has. With no pad yet, the panel shows Xbox's: the
   * standard mapping is laid out after its pad. */
  function family(pad) {
    const id = (pad && pad.id) || '';
    const m = /Vendor:\s*([0-9a-f]{4})\s*Product:\s*([0-9a-f]{4})/i.exec(id)
      || /^([0-9a-f]{1,4})-([0-9a-f]{1,4})-/i.exec(id);
    const vendor = m ? parseInt(m[1], 16) : -1, product = m ? parseInt(m[2], 16) : -1;
    if (vendor === 0x054c || /playstation|dualshock|dualsense/i.test(id)) {
      if (product === 0x0268 || /playstation\(r\)3|dualshock\s*3/i.test(id)) return 'ps3';
      if (product === 0x0ce6 || product === 0x0df2 || /dualsense/i.test(id)) return 'ps5';
      return 'ps4';
    }
    if (vendor === 0x057e || /nintendo|pro controller|joy-con/i.test(id)) return 'switch';
    return 'xbox';
  }

  const url = (file) => (file ? 'prompts/' + file + '.svg' : null);

  /* A binding ({b} a button, {a, d} an axis and its sign) on a standard pad. */
  function pad(x, p) {
    const f = family(p);
    if (x.b !== undefined) return url(BUTTONS[f][x.b]);
    if (x.a > 3) return null;
    const dir = x.a & 1 ? (x.d < 0 ? 'up' : 'down') : (x.d < 0 ? 'left' : 'right');
    return url(STICK[f] + '_stick_' + (x.a < 2 ? 'l' : 'r') + '_' + dir);
  }

  /* A key: by what it types where that is a letter, digit or mark the pack
   * draws (the label m2hle-keys.js shows, so a moved key shows its own
   * letter), else by its code. */
  const CHARS = {
    ',': 'comma', '.': 'period', '/': 'slash_forward', ';': 'semicolon', "'": 'apostrophe',
    '-': 'minus', '=': 'equals', '[': 'bracket_open', ']': 'bracket_close',
  };
  const CODES = {
    ArrowUp: 'arrow_up', ArrowDown: 'arrow_down', ArrowLeft: 'arrow_left', ArrowRight: 'arrow_right',
    Space: 'space', Enter: 'enter', NumpadEnter: 'enter', ShiftLeft: 'shift', ShiftRight: 'shift',
    ControlLeft: 'ctrl', ControlRight: 'ctrl', AltLeft: 'alt', AltRight: 'alt', Tab: 'tab',
    Escape: 'escape', Backspace: 'backspace',
  };
  function key(code, label) {
    if (CODES[code]) return url('keyboard_' + CODES[code]);
    const c = label && label.length === 1 ? label.toLowerCase() : '';
    if (/^[a-z0-9]$/.test(c)) return url('keyboard_' + c);
    return CHARS[c] ? url('keyboard_' + CHARS[c]) : null;
  }

  /* Fill a chip: the icon, named for the mouse and a screen reader, then the
   * remove mark; the name in text if there is no icon or it fails to load. */
  function chip(el, src, name) {
    el.textContent = '';
    if (!src) {
      el.textContent = name + ' ×';
      return;
    }
    const img = document.createElement('img');
    img.className = 'prompt';
    img.src = src;
    img.alt = name;
    img.addEventListener('error', () => { el.textContent = name + ' ×'; });
    el.append(img, ' ×');
  }

  return { family, pad, key, chip };
})();
