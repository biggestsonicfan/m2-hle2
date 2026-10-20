/*
 * What a player kept on the old sites, brought over to this one (Pinboard #881).
 *
 * A browser keeps a site's storage per host, so moving from play.sonicthefighte.rs
 * and srally.sonicthefighte.rs to arcade.sonicthefighte.rs left everything behind:
 * the RPCN and GGPO sign-ins (net/netplay.h's config, m2hle-ggpo.js), the backup
 * RAM (core/backup_ram.h), the arcade's choices (cookies, arcade/stf-arcade.mjs),
 * keys, pads and settings. A returning player looked signed out, on a fresh board.
 *
 * The old hosts still serve one page, m2hle-migrate.html (web/aliases/Caddyfile), and
 * redirect everything else here. In a hidden frame it posts its localStorage and
 * cookies to this origin and to nothing else. They are one site
 * (sonicthefighte.rs), so the frame sees the storage the host had as a page of
 * its own: browsers partition a frame's storage only under another site.
 *
 * Only a key this site does not have yet is copied: what the player did here
 * since the move wins. A host is asked until it has answered once. Then the
 * page reloads if anything came over, because the scripts read their settings as
 * they start. Replays and imported shaders are in IndexedDB and stay where they
 * were.
 *
 * The frame is opened only after a CORS probe that refuses redirects. A host that
 * still answers with a plain redirect (before web/aliases is deployed) is not
 * framed, so the frame never loads this page and its emulator.
 */
'use strict';

(() => {
  /* index.html's <meta name="m2hle-migrate">: this site's origin, then the old ones. */
  const meta = document.querySelector('meta[name="m2hle-migrate"]');
  const [HERE, ...OLD] = meta ? meta.content.split(/\s+/).filter(Boolean) : [];
  const PAGE = '/m2hle-migrate.html';
  const DONE = 'm2hle.migrated.';            /* + host: that host has answered */
  const WAIT_MS = 5000;

  if (!HERE || location.origin !== HERE || window.top !== window) return;
  let todo;
  try {
    todo = OLD.filter((o) => !localStorage.getItem(DONE + new URL(o).hostname));
  } catch (e) {
    return;                                  /* storage off: nothing to keep it in */
  }
  if (!todo.length) return;

  const cookieNames = () => new Set(document.cookie.split('; ').filter(Boolean)
                                      .map((c) => c.slice(0, c.indexOf('='))));

  /* What one host sent: the count of keys and cookies taken. */
  function take(data) {
    let n = 0;
    for (const [k, v] of Object.entries(data.items || {})) {
      if (typeof v !== 'string' || k.startsWith(DONE) || localStorage.getItem(k) !== null) continue;
      try { localStorage.setItem(k, v); n++; } catch (e) { /* over quota: keep the rest */ }
    }
    const have = cookieNames();
    for (const c of String(data.cookies || '').split('; ').filter(Boolean)) {
      const name = c.slice(0, c.indexOf('='));
      if (!name || name.startsWith('__') || have.has(name)) continue;
      document.cookie = `${c}; Max-Age=${365 * 24 * 3600}; Path=/; SameSite=Lax; Secure`;
      n++;
    }
    return n;
  }

  function ask(origin) {
    return fetch(origin + PAGE, { mode: 'cors', redirect: 'error', cache: 'no-store' })
      .then((r) => r.ok ? new Promise((resolve) => {
        const frame = document.createElement('iframe');
        const end = (n) => {
          clearTimeout(timer);
          window.removeEventListener('message', heard);
          frame.remove();
          resolve(n);
        };
        const heard = (e) => {
          if (e.origin !== origin || e.source !== frame.contentWindow || !e.data || e.data.m2hleMigrate !== 1) return;
          const n = take(e.data);
          localStorage.setItem(DONE + new URL(origin).hostname, String(Date.now()));
          end(n);
        };
        const timer = setTimeout(() => end(0), WAIT_MS);
        window.addEventListener('message', heard);
        frame.hidden = true;
        frame.src = origin + PAGE;
        document.documentElement.appendChild(frame);
      }) : 0)
      .catch(() => 0);                       /* not deployed there (yet) */
  }

  Promise.all(todo.map(ask)).then((counts) => {
    if (counts.some((n) => n > 0)) location.reload();
  });
})();
