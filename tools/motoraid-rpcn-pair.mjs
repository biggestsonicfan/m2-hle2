#!/usr/bin/env node
/*
 * motoraid-rpcn-pair.mjs — a Motor Raid race over a room on RPCN (Pinboard
 * #1627, profiles/motoraid.h). The owner's board is the link MASTER, the
 * guest's the SLAVE; the Power Sled test of the same room link is
 * powsled-rpcn-pair.mjs.
 *
 * Two headless emulators sign in to the container's own RPCN (127.0.0.1:31313;
 * `python3 /opt/scripts/rpcn-test.py`), one hosts a room, the other joins it,
 * both press Start. The match opens the comm link: each board cold-boots into
 * its role (NETWORK TYPE and CABINET ID set by the profile's hooks). Once the
 * linked attract runs, both boards take two coins and Start, press Start
 * through the course and bike pickers, and hold the throttle.
 * Never run this against a real server.
 *
 *   node tools/motoraid-rpcn-pair.mjs --exe build/m2hle --rom $ROMS_DIR/motoraid.zip \
 *        --users a,b [--seconds 20] [--dir /tmp/run] [--fingerprint HEX]
 *
 * Each account's password is its name (rpcn-test.py account NAME). It passes
 * when the rings are node 1 and 2 of 2, both took packets, nothing was lost,
 * both boards ran 60 frames a second, neither board's link state (_SubMode,
 * 0x518F30) went to 9 (NETWORK ERROR), and the race clock in each board's
 * comm packet (+0x0E) ran on both boards and arrived at the other one.
 * Exit 0 if every sign holds.
 */
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import os from 'node:os';
import { M2Hle, sleep } from './lib/m2hle.mjs';

const args = process.argv.slice(2);
const opt = (name, def) => {
    const i = args.indexOf('--' + name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : def;
};
const EXE     = opt('exe', null);
const ROM     = opt('rom', path.join(process.env.ROMS_DIR ?? '', 'motoraid.zip'));
const USERS   = opt('users', 'test1,test2').split(',');
const SECONDS = Number(opt('seconds', '20'));
const DIR     = opt('dir', fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'motoraid-pair-')));
const SERVER  = opt('server', '127.0.0.1');
const PORT    = Number(opt('port', '31313'));
const FP      = opt('fingerprint', 'C6D3F34CC3B57509F471E9B36F7476D226CDC4DCA2238A0502287356CAE94451');
const ROOM_PASS = opt('room-pass', 'raidpair');   /* 8 bytes: keeps strangers out */

if (SERVER !== '127.0.0.1' && SERVER !== 'localhost') {
    console.error('motoraid-rpcn-pair: only the container\'s own RPCN (127.0.0.1)');
    process.exit(2);
}

/* A port nobody is listening on, from the OS. */
const freePort = () => new Promise((res, rej) => {
    const s = net.createServer();
    s.once('error', rej);
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
});

const le = (hex, n) => {
    let v = 0;
    for (let i = n - 1; i >= 0; i--) v = v * 256 + parseInt(hex.substr(i * 2, 2), 16);
    return v;
};

const read = async (c, ranges) => (await c.rpc('read_many', { ranges }))
    .data.map((x) => (typeof x === 'string' ? x : x.data ?? x.hex));

/* _SubMode of _InitCommunication: 9 is NETWORK ERROR. The race clock (frames
 * since the start) is the halfword at +0x0E of a board's comm packet: its own in the SEND slot, the other board's
 * in RX slot 0. */
const SUBMODE = '0x00518F30', SEND = 0x01A12000, RX0 = 0x01A121C0;
async function link(c) {
    const [sub, mine, theirs] = await read(c, [[SUBMODE, 4], ['0x' + (SEND + 0x0E).toString(16), 2],
                                               ['0x' + (RX0 + 0x0E).toString(16), 2]]);
    return { sub: le(sub, 4), mine: le(mine, 2), theirs: le(theirs, 2) };
}

/* set_input masks (profiles/motoraid.h): COIN1, START, the throttle (B1). */
const COIN = 0x1, START = 0x40, THROTTLE = 0x10000;
async function hold(c, mask, frames) {
    const f0 = (await c.status()).frames;
    await c.rpc('set_input', { held: '0x' + mask.toString(16) });
    await waitFor('a press', async () => (await c.status()).frames >= f0 + frames);
}
const both = (cs, mask, frames) => Promise.all(cs.map((c) => hold(c, mask, frames)));

const np = (c) => c.rpc('netplay_status', { log: 40 });

async function waitFor(what, fn, ms = 60000) {
    const deadline = Date.now() + ms;
    for (;;) {
        const v = await fn();
        if (v) return v;
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(250);
    }
}

async function launch(user) {
    const dir = path.join(DIR, user);
    fs.mkdirSync(dir, { recursive: true });
    const port = await freePort();
    const c = await M2Hle.launch({
        rom: ROM, port, exe: EXE, profile: 'motoraid', region: null, cwd: dir,
        extraArgs: ['--no-nvram', '--net-config', path.join(dir, 'netplay.cfg')],
    });
    c.user = user;
    await c.waitUntilRunning();
    await c.rpc('netplay_connect', { server: SERVER, port: PORT, user, pass: user, fingerprint: FP });
    await waitFor(user + ' online', async () => (await np(c)).state === 'online');
    return c;
}

async function linkUp(host, guest) {
    await host.rpc('netplay_host', { room_pass: ROOM_PASS });
    const room = await waitFor('the room', async () => {
        const s = await np(host);
        return s.room_id !== '0' && s.state_num >= 3 ? s.room_id : null;
    });
    await guest.rpc('netplay_join', { room_id: room, room_pass: ROOM_PASS });
    await waitFor('both peers heard', async () =>
        (await np(host)).peer.heard && (await np(guest)).peer.heard);
    await guest.rpc('netplay_start');
    await waitFor('the guest ready', async () => (await np(host)).peer.ready);
    await host.rpc('netplay_start');
    await waitFor('both links open', async () =>
        (await np(host)).comm?.open && (await np(guest)).comm?.open);
    return room;
}

/* One reading of a client: the board frame and the wall clock with it. */
async function sample(c) {
    const st = await c.status();
    const s = await np(c);
    return { t: Date.now(), frames: st.frames, pace: st.link_pace, comm: s.comm, log: s.log };
}

function checks(name, want, a, b, race) {
    const c = b.comm;
    const secs = (b.t - a.t) / 1000;
    const fps = (b.frames - a.frames) / secs;
    const rows = [
        ['ring node', c.ring.node === want.node && c.ring.nodes === 2, `${c.ring.node} of ${c.ring.nodes}`],
        ['ring up', c.ring.up, c.ring.up],
        ['took packets', c.ring.taken > 0 && c.taken > 0, `ring ${c.ring.taken}, link ${c.taken}`],
        ['nothing before CN 1', c.early === 0 && c.first_send >= c.cn_frame,
            `CN 1 in frame ${c.cn_frame}, first send ${c.first_send}, ${c.early} early`],
        ['none lost', c.overflow === 0 && c.too_big === 0 && c.torn === 0,
            `overflow ${c.overflow}, too big ${c.too_big}, torn ${c.torn}`],
        ['60 Hz', Math.abs(fps - 60) < 1.0, fps.toFixed(2) + ' frames/s'],
        ['no NETWORK ERROR', race.errors === 0, `_SubMode 9 in ${race.errors} of ${race.samples} samples`],
        ['race clock ran', race.ran, `own clock ${race.first} -> ${race.last}`],
        ['peer clock arrived', race.near, `peer's clock off by at most ${race.off} frames`],
    ];
    console.log(`${name} (${c.role}): sent ${c.sent} took ${c.taken} stale ${c.stale}, largest ${c.packed_peak} B; ` +
                `pace ${JSON.stringify(b.pace)}`);
    let ok = true;
    for (const [what, pass, val] of rows) {
        console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${what}: ${val}`);
        ok &&= pass;
    }
    return ok;
}

/* Both boards' link words, counting a NETWORK ERROR screen on either. */
async function watchRace(cs, race) {
    const l = await Promise.all(cs.map(link));
    for (const [i, r] of race.entries()) {
        r.samples++;
        if (l[i].sub === 9) r.errors++;
    }
    return l;
}

/* Coin both cabinets up, start them into the race and open the throttle. */
async function driveToRace(cs, watch) {
    for (let k = 0; k < 2; k++) { await both(cs, COIN, 8); await both(cs, 0, 8); }
    await both(cs, START, 8); await both(cs, 0, 90); await watch();
    for (let k = 0; k < 6; k++) { await both(cs, START, 8); await both(cs, 0, 120); await watch(); }
    await both(cs, THROTTLE, 1);
}

/* One sample of both race clocks: each board's own, and how far its copy
 * of the other's is from the other's own. */
function clockSample(race, l) {
    for (const [i, r] of race.entries()) {
        if (l[i].mine) { r.first ??= l[i].mine; r.last = l[i].mine; }
        /* The other board's clock, read a moment apart: within a second of ours. */
        if (l[i].mine && l[1 - i].mine)
            r.off = Math.max(r.off, Math.abs(l[i].theirs - l[1 - i].mine));
    }
}

function judgeRace(race) {
    for (const r of race) {
        r.ran = r.first !== null && r.last - r.first >= (SECONDS - 5) * 60;
        r.near = r.first !== null && r.off <= 60;
    }
}

async function main() {
    const [hu, gu] = USERS;
    const clients = [];
    const stop = async () => { for (const c of clients) await c.close().catch(() => {}); };
    process.on('SIGINT', () => stop().then(() => process.exit(130)));
    try {
        const host = await launch(hu);  clients.push(host);
        const guest = await launch(gu); clients.push(guest);
        const room = await linkUp(host, guest);
        console.log(`room ${room}: ${hu} owns it, ${gu} joined; both pressed Start`);
        await waitFor('both rings up', async () =>
            (await np(host)).comm.ring.up && (await np(guest)).comm.ring.up, 120000);
        const cs = [host, guest];
        await waitFor('the linked attract', async () => (await host.status()).frames > 2300, 120000);
        const a = [await sample(host), await sample(guest)];
        console.log(`rings up, attract at frames ${a[0].frames} and ${a[1].frames}`);
        const race = cs.map(() => ({ errors: 0, samples: 0, first: null, last: null, off: 0 }));
        const watch = () => watchRace(cs, race);
        await driveToRace(cs, watch);
        const t0 = Date.now();
        while (Date.now() - t0 < SECONDS * 1000) {
            clockSample(race, await watch());
            await sleep(500);
        }
        for (const c of cs) await c.rpc('set_input', { held: '0x0' });
        judgeRace(race);
        const b = [await sample(host), await sample(guest)];
                for (const [i, c] of [host, guest].entries())
            for (const line of b[i].log.filter((l) => /comm link|match|cabinet/.test(l)))
                console.log(`  [${c.user}] ${line}`);
        const ok = checks(hu, { node: 1 }, a[0], b[0], race[0])
                 & checks(gu, { node: 2 }, a[1], b[1], race[1]);
        console.log(ok ? 'PASS' : 'FAIL');
        process.exitCode = ok ? 0 : 1;
    } finally {
        await stop();
    }
}

main().catch((e) => { console.error(e.stack ?? e); process.exitCode = 2; });
