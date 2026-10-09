/*
 * GGPO, the other way to play online (Pinboard #575): the lobby at
 * ggpo.sonicthefighte.rs, where a player signs in, sees who is in the game's
 * channel and challenges them. A match is a cold boot on both boards, then
 * rollback netplay (src/core/emu_ggpo.h). A page has no UDP, so its match
 * rides the lobby's WebSocket; the server relays it.
 *
 * Drawn from the lobby's status (web_ggpo_status in main_web.c, the same JSON
 * the desktop's MCP bridge answers ggpo_lobby with); every button posts a step
 * (web_ggpo_post) and nothing here touches lobby state directly.
 *
 * ?ggpo=ws://127.0.0.1:8080/ws points it at a local lobby for testing.
 */
'use strict';

const m2hleGgpo = (() => {
  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const KEY = 'm2hle.ggpo';     /* { user, pass }: this origin's localStorage, as RPCN's sign-in is */
  const views = ['gg-off', 'gg-busy', 'gg-signin', 'gg-twitch', 'gg-channel', 'gg-match'];

  let M = null;
  let st = null;
  let open = false;
  let saved = null;             /* the stored sign-in, or null */
  let triedSaved = false;       /* offered once a connection; a refusal is shown, not retried */
  let lastStage = '';
  let pending = null;           /* what was typed, stored once the server takes it */

  function withString(text, fn) {
    const n = M.lengthBytesUTF8(text) + 1, p = M._malloc(n);
    M.stringToUTF8(text, p, n);
    try { return fn(p); } finally { M._free(p); }
  }

  function post(kind, a = '', b = '') {
    withString(kind, (k) => withString(a, (ap) => withString(b, (bp) => M._web_ggpo_post(k, ap, bp))));
    poll();
  }

  function load() {
    try { saved = JSON.parse(localStorage.getItem(KEY) || 'null'); } catch (e) { saved = null; }
    if (!saved || !saved.user) saved = null;
  }

  function store(user, pass) {
    saved = user ? { user, pass } : null;
    try {
      if (saved) localStorage.setItem(KEY, JSON.stringify(saved));
      else localStorage.removeItem(KEY);
    } catch (e) { /* private mode: kept for this visit only */ }
  }

  /* ---- What to show ------------------------------------------------------- */

  function view() {
    switch (st.stage) {
      case 'off':
      case 'failed':     return 'gg-off';
      case 'connecting': return 'gg-busy';
      case 'connected':  return st.twitch_code ? 'gg-twitch' : 'gg-signin';
      case 'signed_in':  return 'gg-busy';
      case 'channel':    return 'gg-channel';
      case 'match':      return 'gg-match';
      default:           return 'gg-busy';
    }
  }

  function setText(id, text) { const e = $(id); if (e.textContent !== text) e.textContent = text; }

  function renderUsers() {
    const ul = $('gg-users');
    ul.replaceChildren();
    const others = (st.users || []).filter((u) => u.name !== st.user);
    for (const u of others) {
      const li = document.createElement('li');
      const name = document.createElement('span');
      name.textContent = u.name + (u.state && u.state !== 'idle' ? ` (${u.state})` : '');
      li.append(name);
      if (u.state === 'idle' && !st.challenging && !st.challenged_by) {
        const b = document.createElement('button');
        b.className = 'tool';
        b.textContent = 'Challenge';
        b.addEventListener('click', () => post('challenge', u.name));
        li.append(b);
      }
      ul.append(li);
    }
    $('gg-users-empty').hidden = others.length > 0;
  }

  function renderChannel() {
    setText('gg-me', st.user);
    setText('gg-game', st.game);
    renderUsers();
    $('gg-incoming').hidden = !st.challenged_by;
    setText('gg-incoming-text', st.challenged_by ? `${st.challenged_by} challenges you.` : '');
    $('gg-outgoing').hidden = !st.challenging;
    setText('gg-outgoing-text', st.challenging ? `Waiting for ${st.challenging} to answer...` : '');
    const chat = $('gg-chat');
    const lines = (st.chat || []).join('\n');
    if (chat.textContent !== lines) { chat.textContent = lines; chat.scrollTop = chat.scrollHeight; }
  }

  function renderMatch() {
    setText('gg-opponent', st.opponent || '?');
    const how = !st.peer_known ? 'finding each other' : st.direct ? 'direct' : 'through the server';
    setText('gg-link', `Link: ${how}.`);
  }

  function render() {
    if (!st || !open) return;
    const v = view();
    for (const id of views) $(id).hidden = id !== v;
    setText('gg-error', st.error || '');
    $('gg-error').hidden = !st.error;
    if (v === 'gg-twitch') {
      setText('gg-twitch-code', st.twitch_code);
      $('gg-twitch-link').href = /^https:\/\//.test(st.twitch_uri) ? st.twitch_uri : 'https://www.twitch.tv/activate';
    }
    $('gg-twitch-start').hidden = !st.twitch;
    if (v === 'gg-signin' && saved && !$('gg-user').value) $('gg-user').value = saved.user;
    if (v === 'gg-channel') renderChannel();
    if (v === 'gg-match') renderMatch();
  }

  /* ---- The steps that take no click --------------------------------------- */

  /* A post polls again at once, and so comes back here: the stage is noted
   * before anything is posted, or each step would post itself again. */
  function advance() {
    const prev = lastStage;
    lastStage = st.stage;
    if (st.stage === 'connected' && saved && !triedSaved && !st.twitch_code) {
      triedSaved = true;
      post('login', saved.user, saved.pass || '');
    } else if (st.stage === 'signed_in' && prev !== 'signed_in') {
      if (pending) { store(pending.user, pending.pass); pending = null; }
      post('join');
    }
    if (st.stage === 'off' || st.stage === 'failed') triedSaved = false;
    /* Fold the panel away when the match starts, so it does not cover the game. */
    if (st.stage === 'match' && prev !== 'match' && open) toggle(false);
  }

  function poll() {
    if (!M) return;
    st = JSON.parse(M.UTF8ToString(M._web_ggpo_status()));
    if (!st.ok) return;
    advance();
    render();
    $('btn-ggpo-label').textContent = st.stage === 'match' ? 'GGPO match' : 'GGPO lobby';
  }

  function connect() {
    triedSaved = false;
    post('connect', params.get('ggpo') || '');
  }

  function signIn(kind) {
    const user = $('gg-user').value.trim(), pass = $('gg-pass').value;
    if (!user || !pass) { setText('gg-error', 'A name and a password, please.'); $('gg-error').hidden = false; return; }
    pending = $('gg-remember').checked ? { user, pass } : null;
    post(kind, user, pass);
  }

  function toggle(want) {
    open = want === undefined ? !open : want;
    $('ggpo').hidden = !open;
    $('btn-ggpo').setAttribute('aria-expanded', String(open));
    if (open) {
      for (const id of ['online', 'controls']) if (!$(id).hidden) $(id === 'online' ? 'np-close' : 'pad-close').click();
      if (st && (st.stage === 'off' || st.stage === 'failed')) connect();
      poll();
    } else {
      $('canvas').focus();
    }
  }

  function wire() {
    $('btn-ggpo').addEventListener('click', () => toggle());
    $('gg-close').addEventListener('click', () => toggle(false));
    $('btn-online').addEventListener('click', () => toggle(false));
    $('btn-controls').addEventListener('click', () => toggle(false));
    $('gg-connect').addEventListener('click', connect);
    $('gg-login').addEventListener('click', () => signIn('login'));
    $('gg-signup').addEventListener('click', () => signIn('signup'));
    $('gg-twitch-start').addEventListener('click', () => post('twitch'));
    $('gg-pass').addEventListener('keydown', (e) => { if (e.key === 'Enter') signIn('login'); });
    $('gg-accept').addEventListener('click', () => post('accept'));
    $('gg-decline').addEventListener('click', () => post('decline'));
    $('gg-cancel').addEventListener('click', () => post('cancel'));
    $('gg-end').addEventListener('click', () => post('end'));
    $('gg-signout').addEventListener('click', () => { store(null); post('disconnect'); });
    $('gg-say').addEventListener('keydown', (e) => {
      if (e.key !== 'Enter' || !e.target.value.trim()) return;
      post('chat', e.target.value.trim());
      e.target.value = '';
    });
  }

  function onReady(module) {
    M = module;
    if (!M._web_ggpo_status || !JSON.parse(M.UTF8ToString(M._web_ggpo_status())).ok) return;   /* no GGPO in this build */
    load();
    wire();
    setInterval(poll, 250);
  }

  /* The game has loaded: a challenge can start a cold boot now. */
  function onGame() {
    if (!M || !M._web_ggpo_status || !JSON.parse(M.UTF8ToString(M._web_ggpo_status())).ok) return;
    $('btn-ggpo').hidden = false;
  }

  return { onReady, onGame, toggle, status: () => st };
})();
