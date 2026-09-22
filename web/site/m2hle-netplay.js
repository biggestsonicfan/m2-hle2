/*
 * Playing online: the sign-in, the lobby and the room (WEB-NETPLAY.md, section
 * 5). Drawn from a snapshot the emulator hands over a few times a second
 * (web_netplay_status in main_web.c); every button posts a command and nothing
 * here touches netplay state directly, which is how the desktop's netplay
 * window is written too.
 *
 * The connection goes through the gateway on rpcn.sonicthefighte.rs, because a
 * browser cannot open the sockets RPCN speaks. ?gw=ws://localhost:8787/gw points
 * it at a local gateway for testing.
 */
'use strict';

const m2hleNetplay = (() => {
  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const views = ['np-signin', 'np-twitch', 'np-busy', 'np-lobby', 'np-room', 'np-failed'];
  const TWITCH_STARTING = 1, TWITCH_WAITING = 2;
  const ACCOUNT_WORKING = 1, ACCOUNT_DONE = 2, ACCOUNT_FAILED = 3;

  let M = null;                 /* Module, once the exports exist */
  let st = null;                /* the last snapshot */
  let logFrom = 0;
  let open = false;
  let lastSearch = 0;
  let pendingCreate = null;     /* the account being created, to sign in with after */
  let delayChoice = 'auto';
  let resending = false;
  let dismissedFailure = '';
  let lastState = '';           /* the state at the previous poll */

  /* ---- Calling into the emulator ----------------------------------------------- */

  function withString(text, fn) {
    const n = M.lengthBytesUTF8(text) + 1, p = M._malloc(n);
    M.stringToUTF8(text, p, n);
    try { return fn(p); } finally { M._free(p); }
  }

  /* post('connect', { npid, password }) -> stage the fields, queue the command. */
  function post(cmd, fields = {}) {
    M._web_netplay_begin();
    for (const [k, v] of Object.entries(fields)) {
      withString(k, (kp) => withString(String(v), (vp) => M._web_netplay_set(kp, vp)));
    }
    withString(cmd, (cp) => M._web_netplay_post(cp));
    poll();
  }

  function status() {
    const s = JSON.parse(M.UTF8ToString(M._web_netplay_status(logFrom >>> 0)));
    if (typeof s.log_count === 'number') logFrom = s.log_count;
    return s;
  }

  /* ---- Round trip to the gateway ------------------------------------------------
   * The gateway echoes a datagram addressed to 0.0.0.0:0; web_socket.h consumes
   * the echo and keeps the times. In the lobby this is the only leg there is --
   * nobody has joined yet -- so the frame delay assumes the opponent is about as
   * far away. Once somebody is in the room the emulator pings them directly
   * (netplay_rtt_t in netplay.h) and the room shows that instead: peerMs(). */
  function ping() {
    const socks = (M && M.m2ws && M.m2ws.socks) || {};
    for (const s of Object.values(socks)) {
      if (!s.dgram || s.state !== 1) continue;
      const b = new ArrayBuffer(14);
      new DataView(b).setFloat64(6, performance.now());
      try { s.ws.send(b); } catch (e) { /* closed under us */ }
    }
  }

  function rttMs() {
    const socks = (M && M.m2ws && M.m2ws.socks) || {};
    for (const s of Object.values(socks)) {
      if (!s.dgram || !s.rtt.length) continue;
      const sorted = [...s.rtt].sort((a, b) => a - b);
      return sorted[sorted.length >> 1];
    }
    return null;
  }

  /* Frames of input delay for a match this machine hosts: enough to cover a
   * one-way trip between the two players, taken as one round trip to the
   * gateway (half there, half on to the other player), plus one frame of slack. */
  function autoDelay() {
    const rtt = rttMs();
    if (rtt === null) return 2;
    return Math.max(2, Math.min(6, Math.ceil(rtt / 16.7) + 1));
  }

  /* Round trip to a member of the room, peer to peer, or null before the first
   * answer (or from a build that does not answer pings). */
  function peerMs(ms) { return typeof ms === 'number' ? ms : null; }

  /* Frames of input delay a round trip needs: half of it each way, plus a frame
   * of slack, as autoDelay reckons it. */
  function framesFor(rtt) { return Math.ceil(rtt / 2 / 16.7) + 1; }

  function quality(rtt) {
    if (rtt === null) return 'measuring…';
    const ms = Math.round(rtt) + ' ms';
    if (rtt < 60) return 'good (' + ms + ')';
    if (rtt < 120) return 'okay (' + ms + ')';
    return 'far (' + ms + ') — expect some delay';
  }

  /* ---- Drawing ---------------------------------------------------------------- */

  function show(view) {
    for (const v of views) $(v).hidden = v !== view;
  }

  function setText(id, text) {
    const el = $(id);
    if (el.textContent !== text) el.textContent = text;
  }

  function errorText(id, text) {
    setText(id, text || '');
    $(id).hidden = !text;
  }

  function render() {
    if (!st) return;
    const tw = st.twitch || {};
    const state = st.state;
    $('np-empty').hidden = !st.empty_room;

    /* The state of the game online, in a line. It sits on the menu's own item, so
     * with the menu closed it is what the menu button says it is holding (the
     * button also turns green: m2hle.css, .online-btn.live). */
    let pill = '';
    const peerRtt = peerMs(st.room && st.room.peer_rtt_ms);
    if (state === 'playing') pill = 'Online: playing ' + (st.room.peer || '') + (peerRtt === null ? '' : ' · ' + peerRtt + ' ms');
    else if (state === 'watching') pill = 'Online: watching';
    else if (state === 'waiting at the barrier') pill = 'Online: starting…';
    else if (state === 'in a room') pill = st.room.peer_heard ? 'Online: ' + st.room.peer + ' is here' : 'Online: waiting';
    else if (state === 'online') pill = 'Online';
    setText('btn-online-label', pill || 'Play online');
    $('btn-online').classList.toggle('live', !!pill);
    if ($('btn-menu')) $('btn-menu').title = pill || 'Menu';

    if (tw.state === TWITCH_STARTING || tw.state === TWITCH_WAITING) {
      show('np-twitch');
      const waiting = tw.state === TWITCH_WAITING && tw.code;
      $('np-twitch-code-box').hidden = !waiting;
      setText('np-twitch-lead', waiting ? 'Almost there. Open Twitch, check the code matches this one, and approve it.'
                                        : 'Asking Twitch for a sign-in code…');
      if (waiting) {
        setText('np-twitch-code', tw.code);
        const a = $('np-twitch-link');
        if (tw.uri && a.getAttribute('href') !== tw.uri) a.setAttribute('href', tw.uri);
        a.hidden = !tw.uri;
      }
      return;
    }

    if (state === 'connecting') {
      show('np-busy');
      setText('np-busy-text', 'Signing in' + (st.npid ? ' as ' + st.npid : '') + '…');
      return;
    }

    if (state === 'failed') {
      const why = st.error || 'The connection failed.';
      if (dismissedFailure !== why) {
        show('np-failed');
        setText('np-failed-text', friendly(why));
        return;
      }
    }

    if (state === 'online') {
      show('np-lobby');
      renderLobby();
      return;
    }
    if (state === 'in a room' || state === 'waiting at the barrier' || state === 'playing' || state === 'watching') {
      show('np-room');
      renderRoom();
      return;
    }

    /* Off (or a failure the player has already read): the sign-in. */
    show('np-signin');
    renderSignin();
  }

  /* The netcode's messages are written for the desktop's netplay window, which
   * calls the e-mail code a "token". Say it the way this page does. */
  function friendly(why) {
    if (/verifies accounts by e-mail and no token/.test(why)) {
      return 'This server checks new accounts by e-mail. It has sent a code to the address you signed up with: ' +
             'go Back, enter it under "The server e-mailed me a code", and sign in again.';
    }
    if (/verification token was refused/.test(why)) {
      return 'That code was not accepted. Check it against the e-mail, or go Back and have it sent again.';
    }
    return why;
  }

  function selectTab(t) {
    for (const u of ['signin', 'create']) {
      $('np-tab-' + u).setAttribute('aria-selected', String(u === t));
      $('np-form-' + u).hidden = u !== t;
    }
  }

  function renderSignin() {
    const tw = st.twitch || {};
    const acct = st.account || {};
    const returning = ((tw.signed_in && tw.npid) || (st.npid && st.has_password)) && !/token/.test(dismissedFailure);
    $('np-returning').hidden = !returning;
    $('np-fresh').hidden = !!returning;
    if (returning) setText('np-returning-name', tw.signed_in && tw.npid ? tw.npid : st.npid);
    errorText('np-twitch-error', tw.state === 4 /* failed */ ? 'Twitch sign-in did not finish: ' + tw.error : '');

    /* A resend is only answered once it has been seen running: until the queued
     * command reaches the emulator, the state is still the sign-up's DONE. */
    if (resending === 1 && acct.state === ACCOUNT_WORKING) resending = 2;
    if (acct.state === ACCOUNT_WORKING) {
      setText('np-create-status', 'Creating your account…');
    } else if (acct.state === ACCOUNT_FAILED) {
      setText('np-create-status', '');
      errorText('np-create-error', acct.error);
    } else if (acct.state === ACCOUNT_DONE && pendingCreate) {
      /* Made. Move to the sign-in form with the details filled in and the code
       * box open, and try signing in: a server that verifies by e-mail refuses
       * that first try and has mailed a code, one that does not lets it
       * through. Either way the next step is already on screen. */
      const c = pendingCreate;
      pendingCreate = null;
      setText('np-create-status', '');
      $('np-signin-name').value = c.npid;
      $('np-signin-password').value = c.password;
      $('np-code-box').open = true;
      selectTab('signin');
      setText('np-signin-note', 'Account created. If an e-mail with a code arrives, enter the code below.');
      $('np-signin-note').hidden = false;
      post('connect', { npid: c.npid, password: c.password });
    } else if (acct.state === ACCOUNT_DONE && resending === 2) {
      resending = false;
      setText('np-signin-note', 'Sent. Check your e-mail (and its spam folder) for the code.');
      $('np-signin-note').hidden = false;
    } else if (acct.state === ACCOUNT_FAILED && resending === 2) {
      resending = false;
      setText('np-signin-note', acct.error);
      $('np-signin-note').hidden = false;
    }
  }

  function renderLobby() {
    setText('np-me', st.npid);
    const rtt = rttMs();
    setText('np-quality', 'Connection: ' + quality(rtt));
    const auto = autoDelay();
    setText('np-delay-auto', 'Automatic (' + auto + ' frames)');

    const list = $('np-rooms');
    const rows = st.rooms || [];
    const playable = rows.filter((r) => !r.why && r.members < r.slots);
    setText('np-rooms-empty-text', rows.length ? '' : 'Nobody is waiting right now. Create a match and someone can join you.');
    $('np-rooms-empty').hidden = rows.length > 0;
    setText('np-rooms-count', playable.length ? playable.length + ' waiting' : '');

    /* Rebuild only when the rows changed, so a button is not replaced under a click. */
    const key = JSON.stringify(rows.map((r) => [r.id, r.owner, r.members, r.why, r.password]));
    if (list.dataset.key === key) return;
    list.dataset.key = key;
    list.textContent = '';
    for (const r of rows) {
      const li = document.createElement('li');
      li.className = 'np-room' + (r.why ? ' np-room-off' : '');
      const who = document.createElement('span');
      who.className = 'np-who';
      who.textContent = r.owner || 'Someone';
      const what = document.createElement('span');
      what.className = 'np-what';
      what.textContent = r.why ? r.why
                       : r.members >= r.slots ? 'is already playing'
                       : r.password ? 'is waiting (private match)' : 'is waiting';
      li.append(who, what);
      if (!r.why && r.members < r.slots) {
        const b = document.createElement('button');
        b.className = 'tool primary';
        b.textContent = 'Play';
        b.addEventListener('click', () => join(r));
        li.append(b);
      }
      list.append(li);
    }
  }

  function renderRoom() {
    const r = st.room;
    const members = r.members || [];
    const peer = r.peer || 'your opponent';
    const playing = st.state === 'playing';
    const watching = st.state === 'watching';
    const syncing = st.state === 'waiting at the barrier';
    const nameOf = (side) => (members.find((m) => m.side === side) || {}).npid || '?';
    let lead;
    if (playing) lead = 'Playing ' + peer + '.';
    else if (watching) lead = 'Watching ' + nameOf(0) + ' vs ' + nameOf(1) + '.';
    else if (syncing) lead = r.peer_heard ? 'Starting the match with ' + peer + '…' : 'Starting…';
    else if (members.length < 2) lead = r.host ? 'Waiting for players to join…' : 'Joining…';
    else if (r.auto_start_s) lead = 'Next match in ' + r.auto_start_s + ' s.';
    else if (r.phase === 'match') lead = 'A match is being played.';
    else if (r.peer_ready && !r.ready) lead = 'Others are ready to play.';
    else lead = members.length + ' in the room (up to ' + r.max + ').';
    setText('np-room-lead', lead);

    /* The line, front first: who is up, who is on which side. */
    const list = $('np-members');
    list.textContent = '';
    for (const m of members) {
      const li = document.createElement('li');
      li.className = 'np-room' + (m.me ? ' np-me' : '');
      const who = document.createElement('span');
      who.className = 'np-who';
      who.textContent = (m.line >= 0 ? (m.line + 1) + '. ' : '') + m.npid + (m.me ? ' (you)' : '');
      const what = document.createElement('span');
      what.className = 'np-what';
      what.textContent = (m.side === 0 ? '1P' : m.side === 1 ? '2P'
                         : m.watch ? 'watching' : m.ready ? 'ready' : m.heard ? 'waiting' : 'connecting…')
                       + ' · ' + m.wins + '-' + (m.games - m.wins)
                       + (!m.me && peerMs(m.rtt_ms) !== null ? ' · ' + m.rtt_ms + ' ms' : '');
      if (!m.me && peerMs(m.rtt_ms) !== null) what.title = 'Round trip between you and ' + m.npid + ', measured directly';
      li.append(who, what);
      list.append(li);
    }

    $('np-start').hidden = playing || watching || syncing || members.length < 2;
    setText('np-start', r.ready ? 'Not ready' : 'Ready');
    $('np-room-hint').hidden = playing || watching;
    $('np-stop').hidden = !playing && !syncing && !watching;
    setText('np-stop', watching ? 'Stop watching' : 'End match');

    const bits = [];
    const rtt = peerMs(r.peer_rtt_ms);
    if (rtt !== null && !watching) bits.push('connection to ' + peer + ': ' + quality(rtt));
    if (playing || syncing) {
      bits.push('input delay ' + st.delay + ' frames');
      if (st.stalls) bits.push(st.stalls + ' waits for the other player');
    }
    setText('np-room-status', bits.join(' · '));
    /* The delay was fixed when the room was made, before anyone's distance was
     * known. Say so when the trip turns out longer than it covers. */
    const short = rtt !== null && !watching && st.delay > 0 && framesFor(rtt) > st.delay;
    errorText('np-rtt-warn', short
      ? 'The round trip to ' + peer + ' needs about ' + framesFor(rtt) + ' frames of input delay and this room uses ' +
        st.delay + ', so the game will pause now and then to wait. A room made with a longer delay plays smoother.'
      : '');
    errorText('np-desync', st.desync === null ? ''
      : 'The two games stopped matching at frame ' + st.desync + '. What you see from here on may differ from ' +
        'what ' + peer + ' sees. Leave and start a new match.');
  }

  /* ---- Actions ---------------------------------------------------------------- */

  function join(room) {
    let pw = '';
    if (room.password) {
      pw = window.prompt('That is a private match. Password:') || '';
      if (!pw) return;
    }
    post('join', { room_id: room.id, room_password: pw });
  }

  function hostDelay() {
    return delayChoice === 'auto' ? autoDelay() : Number(delayChoice);
  }

  function wire() {
    $('btn-online').addEventListener('click', () => toggle());
    $('np-close').addEventListener('click', () => toggle(false));

    $('np-twitch').querySelector('.np-cancel').addEventListener('click', () => post('twitch_cancel'));
    $('np-twitch-copy').addEventListener('click', () => {
      navigator.clipboard.writeText($('np-twitch-code').textContent).then(
        () => setText('np-twitch-copy', 'Copied'), () => {});
    });

    $('np-twitch-start').addEventListener('click', () => { dismissedFailure = ''; post('twitch_start'); });
    $('np-continue').addEventListener('click', () => {
      dismissedFailure = '';
      const tw = st && st.twitch;
      if (tw && tw.signed_in) post('twitch_start');     /* reuses the stored login */
      else post('connect');
    });
    $('np-not-me').addEventListener('click', signOut);

    for (const t of ['signin', 'create']) $('np-tab-' + t).addEventListener('click', () => selectTab(t));

    $('np-resend').addEventListener('click', () => {
      const npid = $('np-signin-name').value.trim(), password = $('np-signin-password').value;
      if (!npid || !password) {
        setText('np-signin-note', 'Fill in your name and password first.');
        $('np-signin-note').hidden = false;
        return;
      }
      resending = 1;
      setText('np-signin-note', 'Asking the server to send the e-mail again…');
      $('np-signin-note').hidden = false;
      post('resend_token', { npid, password });
    });

    $('np-form-signin').addEventListener('submit', (e) => {
      e.preventDefault();
      dismissedFailure = '';
      post('connect', {
        npid: $('np-signin-name').value.trim(),
        password: $('np-signin-password').value,
        token: $('np-signin-token').value.trim(),
      });
    });

    $('np-form-create').addEventListener('submit', (e) => {
      e.preventDefault();
      errorText('np-create-error', '');
      const npid = $('np-create-name').value.trim();
      const password = $('np-create-password').value;
      const email = $('np-create-email').value.trim();
      if (!/^[A-Za-z0-9_-]{3,16}$/.test(npid)) {
        errorText('np-create-error', 'Names are 3 to 16 letters, digits, - or _.');
        return;
      }
      if (password.length < 4) { errorText('np-create-error', 'Choose a longer password.'); return; }
      dismissedFailure = '';
      pendingCreate = { npid, password };
      post('create_account', { npid, password, email });
    });

    $('np-create-match').addEventListener('click', () => {
      post('host', { frame_delay: hostDelay(), room_password: $('np-private').value,
                     max_players: $('np-size').value });
    });
    $('np-delay').addEventListener('change', (e) => { delayChoice = e.target.value; });
    $('np-signout').addEventListener('click', signOut);

    $('np-start').addEventListener('click', () => {
      post(st && st.room.ready ? 'stop' : 'start');
      $('canvas').focus();
    });
    $('np-stop').addEventListener('click', () => post('stop'));
    $('np-leave').addEventListener('click', () => post('leave'));

    $('np-failed-back').addEventListener('click', () => {
      const why = st ? (st.error || 'The connection failed.') : '';
      dismissedFailure = why;
      if (/token/.test(why)) {
        /* Back to where the code goes: the sign-in form, box open, cursor in it. */
        $('np-returning').hidden = true;
        $('np-fresh').hidden = false;
        selectTab('signin');
        $('np-code-box').open = true;
        render();
        $('np-signin-token').focus();
        return;
      }
      render();
    });
  }

  function signOut() {
    M._web_netplay_signout();
    for (const id of ['np-signin-name', 'np-signin-password', 'np-signin-token']) $(id).value = '';
    poll();
  }

  function toggle(want) {
    open = want === undefined ? !open : want;
    $('online').hidden = !open;
    $('btn-online').setAttribute('aria-expanded', String(open));
    if (open) {
      render();
      const first = $('online').querySelector('section:not([hidden]) button:not([hidden]), section:not([hidden]) input');
      if (first) first.focus();
    } else {
      $('canvas').focus();
    }
  }

  /* ---- The loop -------------------------------------------------------------- */

  function poll() {
    if (!M) return;
    st = status();
    /* The lobby's list goes stale on its own: ask again every few seconds while
     * someone is looking at it. */
    if (open && st.state === 'online' && !st.search_pending && performance.now() - lastSearch > 4000) {
      lastSearch = performance.now();
      M._web_netplay_begin();
      withString('search', (cp) => M._web_netplay_post(cp));
    }
    /* Fold the panel away when the match starts, so it does not cover the game;
     * the bar still says who you are playing. Once, on the way INTO playing:
     * doing it on every poll while playing shut the panel again a quarter of a
     * second after the player opened it to end the match or leave. */
    const running = st.state === 'playing' || st.state === 'watching';
    if (running && lastState !== st.state && open) toggle(false);
    lastState = st.state;
    render();
  }

  /* A tab in the background keeps its board running for every game, a match
   * included: keepRunning in m2hle-page.js. */

  function onReady(module) {
    M = module;
    const gw = params.get('gw');
    if (gw) withString(gw, (p) => M._web_netplay_set_gateway(p));
    wire();
    setInterval(poll, 250);
    setInterval(ping, 2000);
    poll();
  }

  /* The game has loaded: online play can be offered. */
  function onGame() {
    $('btn-online').hidden = false;
  }

  return { onReady, onGame, toggle, status: () => st, rtt: rttMs };
})();
