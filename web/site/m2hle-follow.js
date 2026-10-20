/* ?follow=<dir> : this page follows another board one way (core/follow.h, Pinboard #568).
 *
 * <dir> is a folder on THIS site holding what a leader writes (m2hle --follow-out,
 * or the bridge's follow_lead): follow.json names the newest segment, seg-N.sta is
 * its join point and seg-N.feed the inputs, memory writes and frame checks since.
 * The page joins at the newest segment, reads the feed as it grows (Range requests)
 * and hands it to the board, which runs a slice only once the feed holds it. The
 * page's own keys and pads reach nothing: the board reads the leader's input.
 *
 * Only this site's folders are followed, as with ?rom=: a join point is a whole
 * board state, and the page keeps the netplay sign-in in this origin's storage. */
'use strict';

const follow = {
  M: null,
  base: null,
  refused: false,
  seg: 0,           /* the segment joined, 0 before the first */
  at: 0,            /* bytes of its feed handed over */
  busy: false,      /* a poll is in flight */
  said: '',
};
const FOLLOW_POLL_MS = 250;

function followBase() {
  const param = new URLSearchParams(location.search).get('follow');
  if (!param) return;
  const url = new URL(param.endsWith('/') ? param : param + '/', location.href);
  if (url.origin === location.origin) follow.base = url;
  else follow.refused = true;
}
followBase();

function followFetch(name, init) {
  return fetch(new URL(name, follow.base), Object.assign({ cache: 'no-store' }, init)).then((r) => {
    if (!r.ok && r.status !== 206 && r.status !== 416) throw new Error(name + ': ' + r.status);
    return r;
  });
}
const followIndex = () => followFetch('follow.json').then((r) => r.json());

function followGive(fn, bytes) {
  const M = follow.M, ptr = M._malloc(bytes.length || 1);
  M.HEAPU8.set(bytes, ptr);            /* the board frees it */
  return fn(ptr, bytes.length);
}

function followSay(text, level) {
  if (text === follow.said) return;
  follow.said = text;
  m2hleTools.add('follow: ' + text, level);
}

const followStatus = () => JSON.parse(follow.M.UTF8ToString(follow.M._web_follow_status()));

/* Join segment n, or the newest when it has gone (the leader keeps two). */
async function followJoin(n) {
  let r;
  try { r = await followFetch('seg-' + n + '.sta'); } catch (e) {
    const ix = await followIndex();
    if (ix.seg === n) throw e;
    return followJoin(ix.seg);
  }
  const M = follow.M, bytes = new Uint8Array(await r.arrayBuffer());
  if (followGive(M._web_follow_join, bytes) !== 0)
    throw new Error('segment ' + n + ': ' + M.UTF8ToString(M._web_follow_error()));
  follow.seg = n;
  follow.at = 0;
  followSay('joined segment ' + n + ' at frame ' + followStatus().frame);
}

async function followFeed() {
  const r = await followFetch('seg-' + follow.seg + '.feed',
                              { headers: { Range: 'bytes=' + follow.at + '-' } });
  if (r.status === 416) return;
  let bytes = new Uint8Array(await r.arrayBuffer());
  if (r.status === 200) bytes = bytes.subarray(follow.at);   /* a server that ignores Range */
  if (!bytes.length) return;
  follow.at += bytes.length;
  if (followGive(follow.M._web_follow_feed, bytes) !== 0)
    followSay(follow.M.UTF8ToString(follow.M._web_follow_error()), 'warning');
}

async function followPoll() {
  const s = followStatus();
  if (!s.split && !s.ended) return followFeed();
  /* A split joins the newest segment; an END the next, once its state is out. */
  const ix = await followIndex();
  if (s.split) followSay('split from the leader (' + s.why + '), joining again', 'warning');
  if (ix.seg > follow.seg) await followJoin(s.split ? ix.seg : follow.seg + 1);
}

function followTick() {
  if (follow.busy) return;
  follow.busy = true;
  followPoll().catch((e) => followSay(e.message, 'warning'))
              .finally(() => { follow.busy = false; });
}

/* The leader's index, waiting for a leader that has not written one yet. */
function followFirstIndex() {
  return followIndex().catch((e) => {
    followSay('waiting for the leader (' + e.message + ')');
    return new Promise((ok) => setTimeout(ok, 1000)).then(followFirstIndex);
  });
}

/* Before the game loads: the leader's profile picks the ROM files and the hooks. */
function followPrepare(module) {
  follow.M = module;
  if (!follow.base) return Promise.resolve();
  return followFirstIndex().then((ix) => {
    const M = follow.M, p = String(ix.profile || '');
    const n = M.lengthBytesUTF8(p) + 1, sp = M._malloc(n);
    M.stringToUTF8(p, sp, n);
    const rc = M._web_follow_begin(sp);
    M._free(sp);
    if (rc !== 0) throw new Error('this build has no profile "' + p + '"');
  });
}

/* Once the game has loaded: join the newest segment and keep reading. */
function followOnGame() {
  if (!follow.base) return;
  followIndex().then((ix) => followJoin(ix.seg))
               .then(() => setInterval(followTick, FOLLOW_POLL_MS))
               .catch((e) => followSay(e.message, 'warning'));
}

const m2hleFollow = {
  active: follow.base !== null,
  refused: follow.refused,
  prepare: followPrepare,
  onGame: followOnGame,
};
