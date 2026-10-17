#!/usr/bin/env node
/*
 * von-rpcn-pair.mjs — two Virtual-On cabinets linked over a room on RPCN
 * (VIRTUAL-ON-PS3.md, step 5 of the RPCN plan; Pinboard #983).
 *
 * Two headless emulators, one board each, sign in to the container's own RPCN
 * (127.0.0.1:31313; `python3 /opt/scripts/rpcn-test.py`), one hosts a room, the
 * other joins it, both press Start. The room's match opens the comm link on
 * each (netplay_comm_open): the owner's board boots as the MASTER, the guest's
 * as the SLAVE, and the ROM's link traffic goes over the room's P2P socket.
 * Never run this against a real server.
 *
 *   node tools/von-rpcn-pair.mjs --exe build/m2hle --rom $ROMS_DIR/vonj.zip \
 *        --users a,b [--seconds 120] [--dir /tmp/run] [--fingerprint HEX]
 *
 * Each account's password is its name (rpcn-test.py account NAME). Every client
 * gets its own --net-config and working directory under --dir. After the link
 * is up it watches both for --seconds and checks the link's signs: role bytes
 * 1 (owner) and 0, ring nodes 1 and 2 of 2, link_ID 1 and 2, net_flag 1,
 * packets taken both ways, nothing sent before the comm board started, no
 * message lost to the inbox (comm_link overflow), and 60 frames a second of
 * wall time on both boards. Exit 0 if every sign holds.
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
const ROM     = opt('rom', path.join(process.env.ROMS_DIR ?? '', 'vonj.zip'));
const USERS   = opt('users', 'test1,test2').split(',');
const SECONDS = Number(opt('seconds', '120'));
const DIR     = opt('dir', fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'von-pair-')));
const SERVER  = opt('server', '127.0.0.1');
const PORT    = Number(opt('port', '31313'));
const FP      = opt('fingerprint', 'C6D3F34CC3B57509F471E9B36F7476D226CDC4DCA2238A0502287356CAE94451');
const ROOM_PASS = opt('room-pass', 'vonpair1');   /* 8 bytes: keeps strangers out */

if (SERVER !== '127.0.0.1' && SERVER !== 'localhost') {
    console.error('von-rpcn-pair: only the container\'s own RPCN (127.0.0.1)');
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

/* The ROM's own link signs (profiles/vonj.h; det_digest --room-role). */
async function vonSigns(c) {
    const r = await c.rpc('read_many', { ranges: [['0x01D00028', 1], ['0x005770B0', 2],
                                                   ['0x00503A08', 4], ['0x005024F4', 4]] });
    const d = r.data.map((x) => (typeof x === 'string' ? x : x.data ?? x.hex));
    return { role_byte: le(d[0], 1), net_flag: le(d[1], 1), link_id: le(d[1].slice(2), 1),
             w503A08: le(d[2], 4), crecn4: le(d[3], 4) };
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
        rom: ROM, port, exe: EXE, profile: 'vonj', region: null, cwd: dir,
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

function checks(name, want, a, b, signs) {
    const c = b.comm;
    const secs = (b.t - a.t) / 1000;
    const fps = (b.frames - a.frames) / secs;
    const rows = [
        ['role byte', signs.role_byte === want.role_byte, signs.role_byte],
        ['ring node', c.ring.node === want.node && c.ring.nodes === 2, `${c.ring.node} of ${c.ring.nodes}`],
        ['ring up', c.ring.up, c.ring.up],
        ['link_ID', signs.link_id === want.node, signs.link_id],
        ['net_flag', signs.net_flag === 1, signs.net_flag],
        ['took packets', c.ring.taken > 0 && c.taken > 0, `ring ${c.ring.taken}, link ${c.taken}`],
        ['nothing before CN 1', c.early === 0 && c.first_send >= c.cn_frame,
            `CN 1 in frame ${c.cn_frame}, first send ${c.first_send}, ${c.early} early`],
        ['none lost', c.overflow === 0 && c.too_big === 0 && c.torn === 0,
            `overflow ${c.overflow}, too big ${c.too_big}, torn ${c.torn}`],
        ['60 Hz', Math.abs(fps - 60) < 1.0, fps.toFixed(2) + ' frames/s'],
    ];
    console.log(`${name} (${c.role}): sent ${c.sent} took ${c.taken} stale ${c.stale}, largest ${c.packed_peak} B; ` +
                `cRecn+4 0x${signs.crecn4.toString(16)}, 503A08 ${signs.w503A08}; pace ${JSON.stringify(b.pace)}`);
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
        await sleep(SECONDS * 1000);
        const b = [await sample(host), await sample(guest)];
        const signs = [await vonSigns(host), await vonSigns(guest)];
        for (const [i, c] of [host, guest].entries())
            for (const line of b[i].log.filter((l) => /comm link|match|cabinet/.test(l)))
                console.log(`  [${c.user}] ${line}`);
        const ok = checks(hu, { role_byte: 1, node: 1 }, a[0], b[0], signs[0])
                 & checks(gu, { role_byte: 0, node: 2 }, a[1], b[1], signs[1]);
        console.log(ok ? 'PASS' : 'FAIL');
        process.exitCode = ok ? 0 : 1;
    } finally {
        await stop();
    }
}

main().catch((e) => { console.error(e.stack ?? e); process.exitCode = 2; });
