#!/usr/bin/env node
/*
 * powsled-rpcn-pair.mjs — a Power Sled race over a room on RPCN: the room's
 * owner runs the Main unit, the guest rides a sled (Pinboard #1089; the link
 * itself is #1052/#1077, profiles/powsled.h). The Virtual-On test of the same
 * room link is von-rpcn-pair.mjs.
 *
 * Two headless emulators sign in to the container's own RPCN (127.0.0.1:31313;
 * `python3 /opt/scripts/rpcn-test.py`), one hosts a room, the other joins it,
 * both press Start. The match opens the comm link (netplay_comm_open): the
 * owner's board boots as the Main (game_link_role_t MASTER), the guest's as
 * the ride. Once both rings are up it plays the start sequence (README, Power
 * Sled): the Main calls (P1 B2), the ride enters (P1 B1), the Main steps the
 * panel (P2 B2 twice) and starts the race (P2 B3); then the ride holds a pedal.
 * Never run this against a real server.
 *
 *   node tools/powsled-rpcn-pair.mjs --exe build/m2hle --rom $ROMS_DIR/powsled.zip \
 *        --users a,b [--seconds 30] [--dir /tmp/run] [--fingerprint HEX]
 *
 * Each account's password is its name (rpcn-test.py account NAME). Every client
 * gets its own --net-config and working directory under --dir. It passes when
 * the rings are node 1 and 2 of 2, both took packets, nothing was lost to the
 * inbox, both boards ran 60 frames a second, and the race state (0x53E744,
 * 7 = racing) reached 7 on both boards. Exit 0 if every sign holds.
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
const ROM     = opt('rom', path.join(process.env.ROMS_DIR ?? '', 'powsled.zip'));
const USERS   = opt('users', 'test1,test2').split(',');
const SECONDS = Number(opt('seconds', '30'));
const DIR     = opt('dir', fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'powsled-pair-')));
const SERVER  = opt('server', '127.0.0.1');
const PORT    = Number(opt('port', '31313'));
const FP      = opt('fingerprint', 'C6D3F34CC3B57509F471E9B36F7476D226CDC4DCA2238A0502287356CAE94451');
const ROOM_PASS = opt('room-pass', 'sledpair');   /* 8 bytes: keeps strangers out */

if (SERVER !== '127.0.0.1' && SERVER !== 'localhost') {
    console.error('powsled-rpcn-pair: only the container\'s own RPCN (127.0.0.1)');
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

/* The race state: its first byte goes 05 (network check) 06 (entry) 07 (racing). */
const RACE_STATE = '0x0053E744';
async function raceState(c) {
    const r = await c.rpc('read_many', { ranges: [[RACE_STATE, 1]] });
    const d = r.data.map((x) => (typeof x === 'string' ? x : x.data ?? x.hex));
    return le(d[0], 1);
}

/* set_input masks (the ones #1077's two-board runs used): P1 B1 Entry, P1 B2 Call,
 * P2 B2 panel step, P2 B3 start race, P1 RIGHT the right pedal. */
const ENTRY = 0x100, CALL = 0x200, STEP = 0x800, GO = 0x20000, PEDAL = 0x8;
async function press(c, mask, frames = 10) {
    const f0 = (await c.status()).frames;
    await c.rpc('set_input', { held: '0x' + mask.toString(16) });
    await waitFor('a press', async () => (await c.status()).frames >= f0 + frames);
    await c.rpc('set_input', { held: '0x0' });
}

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
        rom: ROM, port, exe: EXE, profile: 'powsled', region: null, cwd: dir,
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
        ['raced', race.peak === 7, `race state peaked at ${race.peak}, now ${race.now}`],
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
        const a = [await sample(host), await sample(guest)];
        console.log(`rings up at frames ${a[0].frames} and ${a[1].frames}`);
        /* The frames of #1077's runs, counted from the board's boot into its role. */
        const at = (c, f) => waitFor('frame ' + f, async () => (await c.status()).frames >= f, 120000);
        for (const [c, f, mask] of [[host, 2400, CALL], [guest, 2700, ENTRY], [host, 3000, STEP],
                                    [host, 3200, STEP], [host, 3600, GO]]) {
            await at(c, f);
            await press(c, mask);
            console.log(`  [${c.user}] frame ${f}: 0x${mask.toString(16)}; race states ${await raceState(host)}, ${await raceState(guest)}`);
        }
        const peak = [0, 0];
        const t0 = Date.now();
        await guest.rpc('set_input', { held: '0x' + PEDAL.toString(16) });
        while (Date.now() - t0 < SECONDS * 1000) {
            for (const [i, c] of [host, guest].entries()) peak[i] = Math.max(peak[i], await raceState(c));
            await sleep(500);
        }
        await guest.rpc('set_input', { held: '0x0' });
        const b = [await sample(host), await sample(guest)];
        const race = [{ peak: peak[0], now: await raceState(host) }, { peak: peak[1], now: await raceState(guest) }];
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
