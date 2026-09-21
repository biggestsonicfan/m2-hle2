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
   * the echo and keeps the times. Only this leg is measurable -- the opponent's
   * is not -- so the frame delay assumes they are about as far away. */
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

    /* The small line in the bottom bar, visible with the panel closed too. */
    let pill = '';
    if (state === 'playing') pill = 'Online: playing ' + (st.room.peer || '');
    else if (state === 'waiting at the barrier') pill = 'Online: starting…';
    else if (state === 'in a room') pill = st.room.peer_heard ? 'Online: ' + st.room.peer + ' is here' : 'Online: waiting';
    else if (state === 'online') pill = 'Online';
    setText('btn-online-label', pill || 'Play online');
    $('btn-online').classList.toggle('live', !!pill);

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
    if (state === 'in a room' || state === 'waiting at the barrier' || state === 'playing') {
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
    const peer = r.peer || 'your opponent';
    const playing = st.state === 'playing';
    const syncing = st.state === 'waiting at the barrier';
    let lead;
    if (playing) lead = 'Playing ' + peer + '.';
    else if (syncing) lead = r.peer_heard ? 'Starting the match with ' + peer + '…' : 'Starting…';
    else if (!r.peer_known && !r.peer_heard) lead = r.host ? 'Waiting for an opponent to join…' : 'Joining…';
    else if (!r.peer_heard) lead = 'Connecting to ' + peer + '…';
    else if (st.room.peer_ready) lead = peer + ' is ready to play.';
    else lead = peer + ' joined.';
    setText('np-room-lead', lead);

    $('np-start').hidden = playing || syncing || !r.peer_heard;
    setText('np-start', st.room.peer_ready ? 'Accept' : 'Start match');
    $('np-room-hint').hidden = playing;
    $('np-stop').hidden = !playing && !syncing;

    const bits = [];
    if (playing || syncing) {
      bits.push('input delay ' + st.delay + ' frames');
      if (st.stalls) bits.push(st.stalls + ' waits for the other player');
    }
    setText('np-room-status', bits.join(' · '));
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
      post('host', { frame_delay: hostDelay(), room_password: $('np-private').value });
    });
    $('np-delay').addEventListener('change', (e) => { delayChoice = e.target.value; });
    $('np-signout').addEventListener('click', signOut);

    $('np-start').addEventListener('click', () => { post('start'); $('canvas').focus(); });
    $('np-stop').addEventListener('click', () => post('stop'));
    $('np-leave').addEventListener('click', () => { post('disconnect'); post('connect'); });

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
    /* Fold the panel away when the match actually starts, so it does not cover
     * the game; the bar still says who you are playing. */
    if (st.state === 'playing' && open && !$('online').dataset.keepOpen) toggle(false);
    render();
  }

  /* ---- A tab in the background ---------------------------------------------------
   * A hidden tab gets no animation frames, so the board would stop and the other
   * player would wait until the session timed out. While a match is on, a
   * worker's timer drives the board instead -- worker timers are not throttled
   * the way a hidden page's are (to be measured per browser: WEB-NETPLAY.md). */
  let worker = null;

  function background() {
    const want = document.hidden && M && M._web_netplay_active();
    if (want && !worker) {
      const src = 'let t=null;onmessage=(e)=>{clearInterval(t);if(e.data)t=setInterval(()=>postMessage(0),16);};';
      worker = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
      worker.onmessage = () => { if (M) M._web_background_tick(); };
      worker.postMessage(1);
      m2hleTools.print('netplay: tab hidden during a match; keeping the game running in the background');
    } else if (!want && worker) {
      worker.postMessage(0);
      worker.terminate();
      worker = null;
    }
  }

  function onReady(module) {
    M = module;
    const gw = params.get('gw');
    if (gw) withString(gw, (p) => M._web_netplay_set_gateway(p));
    wire();
    setInterval(poll, 250);
    setInterval(ping, 2000);
    setInterval(background, 500);
    document.addEventListener('visibilitychange', background);
    poll();
  }

  /* The game has loaded: online play can be offered. */
  function onGame() {
    $('btn-online').hidden = false;
  }

  return { onReady, onGame, toggle, status: () => st, rtt: rttMs };
})();
