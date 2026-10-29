/*
 * No double-tap zoom (Pinboard #1057).
 *
 * m2hle.css asks for it with touch-action (manipulation on the page, none on the
 * canvas and the touch buttons), but Safari on iOS does not always listen: two
 * quick taps, on one touch button or on two (Punch then Kick), or on the picture,
 * still zoom the page. What WebKit does obey is a cancelled touchend, so a tap that
 * ends soon after the last one is cancelled here, before the browser can read the
 * pair as a double tap.
 *
 * Cancelling a touchend also cancels the click the browser would make from it, so
 * a second quick tap on a control is clicked by hand: a menu button or a checkbox
 * still answers every tap. Text fields are left alone (two taps there select a
 * word, and focusing one needs the browser's own click). The touch buttons work on
 * pointer events (m2hle-touch.js) and the board's light guns on sokol's touch
 * events (main_web.c); neither waits for a click, and both still see the touchend.
 *
 * Pinching still zooms where touch-action lets it (the menus, the tools drawer):
 * it is how someone who needs a bigger page gets one.
 */
'use strict';

(() => {
  const QUICK_MS = 350;       /* WebKit's double-tap window is about 300 ms */
  const NEAR_PX = 40;         /* farther apart than this, Safari does not pair them either */
  let last = null;            /* {t, x, y} of the last one-finger tap's end */

  function isText(el) {
    if (!el || !el.closest) return false;
    if (el.closest('textarea, select, [contenteditable=""], [contenteditable="true"]')) return true;
    const input = el.closest('input');
    return !!input && !/^(button|checkbox|color|file|image|radio|range|reset|submit)$/.test(input.type);
  }

  document.addEventListener('touchend', (e) => {
    if (e.touches.length || e.changedTouches.length !== 1) { last = null; return; }
    const p = e.changedTouches[0];
    const prev = last;
    last = { t: e.timeStamp, x: p.clientX, y: p.clientY };
    if (!prev || e.timeStamp - prev.t > QUICK_MS) return;
    /* The touch buttons are far apart on a phone, but a thumb rolling between
     * them is still a "double tap" to Safari: they are cancelled wherever they are. */
    const onButtons = e.target.closest && e.target.closest('.touch');
    if (!onButtons && Math.hypot(p.clientX - prev.x, p.clientY - prev.y) > NEAR_PX) return;
    if (isText(e.target) || !e.cancelable) return;
    e.preventDefault();       /* a third quick tap pairs with this one, and is cancelled too */
    if (!onButtons && e.target instanceof HTMLElement && e.target !== document.getElementById('canvas')) e.target.click();
  }, { passive: false, capture: true });
})();
