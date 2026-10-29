#!/usr/bin/env node
/*
 * motoraid-ggpo-pair.mjs — a GGPO match of Motor Raid between two emulators on
 * this machine, set up by a local ggpo-server lobby (Pinboard #1627;
 * core/emu_twin.h, profiles/motoraid.h).
 *
 * Each emulator runs both boards of the link: board A the MASTER (player 1,
 * the challenger), board B the SLAVE (player 2, who accepts). Only the two
 * players' inputs cross the network. Once GGPO runs, both players take two
 * coins and Start, press Start through the pickers and hold the throttle, as
 * motoraid-rpcn-pair.mjs does. It checks:
 *   - no board's link state (_SubMode 0x518F30) goes to 9 (NETWORK ERROR),
 *     and the race clock (+0x0E of the comm packet) runs on each live board;
 *   - the two machines' "ggpo: frame N board X" lines agree, every one;
 *   - both ends of the twin's link are up and nothing was dropped;
 *   - about 60 frames a second of wall time on each.
 *
 * The lobby must be a local ggpo-server, never the live one:
 *   GGPO_HTTP_PORT=<p> GGPO_UDP_PORT=<u> GGPO_PUBLIC_HOST=127.0.0.1 \
 *       GGPO_DB=/tmp/x/ggpo.db node <ggpo-server>/src/server.mjs
 *   node tools/motoraid-ggpo-pair.mjs --exe build/m2hle --rom $ROMS_DIR/motoraid.zip \
 *        --lobby ws://127.0.0.1:<p>/ws [--seconds 20] [--delay 2] [--relay] [--dir /tmp/run]
 *
 * --relay keeps both sides on the server's relay (no direct path). Exit 0 if
 * every check holds.
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
const LOBBY   = opt('lobby', null);
const SECONDS = Number(opt('seconds', '20'));
const DELAY   = opt('delay', '2');
const RELAY   = args.includes('--relay');
const SYNCLOG = args.includes('--synclog');   /* each check frame's parts, in <dir>/<p>/sync */
const EXTRA   = (opt('args', '') || '').split(' ').filter(Boolean);   /* more m2hle flags, both */
const DIR     = opt('dir', null) ?? fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'motoraid-ggpo-'));

if (!LOBBY || !/^ws:\/\/(127\.0\.0\.1|localhost)[:/]/.test(LOBBY)) {
    console.error('motoraid-ggpo-pair: --lobby ws://127.0.0.1:PORT/ws, a local ggpo-server only');
    process.exit(2);
}

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

/* _SubMode of _InitCommunication (9 is NETWORK ERROR) and the race clock, the
 * halfword at +0x0E of the board's own comm packet (motoraid-rpcn-pair.mjs). */
async function link(c) {
    const r = await c.rpc('read_many', { ranges: [['0x00518F30', 4], ['0x01A1200E', 2]] });
    const d = r.data.map((x) => (typeof x === 'string' ? x : x.data ?? x.hex));
    return { sub: le(d[0], 4), clock: le(d[1], 2) };
}

const COIN = 0x1, START = 0x40, THROTTLE = 0x10000;

const lobby = (c) => c.rpc('ggpo_lobby', {});
const sessionFrames = async (c) => (await lobby(c)).session?.frames ?? 0;

async function waitFor(what, fn, ms = 60000) {
    const deadline = Date.now() + ms;
    for (;;) {
        const v = await fn();
        if (v) return v;
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(250);
    }
}

async function launch(name, user, other, challenger) {
    const dir = path.join(DIR, name);
    fs.mkdirSync(dir, { recursive: true });
    const port = await freePort();
    const c = await M2Hle.launch({
        rom: ROM, port, exe: EXE, profile: 'motoraid', region: null, cwd: dir,
        extraArgs: ['--no-nvram', '--net-config', path.join(dir, 'netplay.cfg'),
                    '--ggpo-lobby', LOBBY, '--ggpo-signup', '--ggpo-user', user, '--ggpo-pass', user + '-pw',
                    ...(challenger ? ['--ggpo-challenge', other] : ['--ggpo-accept', 'any']),
                    '--ggpo-local', String(await freePort()), '--ggpo-delay', DELAY,
                    ...(RELAY ? ['--ggpo-relay'] : []),
                    ...(SYNCLOG ? ['--ggpo-synclog', path.join(dir, 'sync')] : []), ...EXTRA],
    });
    c.name = name;
    c.dir = dir;
    if (SYNCLOG) fs.mkdirSync(path.join(dir, 'sync'), { recursive: true });
    return c;
}

/* m2hle writes m2hle-<id>.log in its working directory. */
const logText = (dir) => fs.readdirSync(dir).filter((f) => /^m2hle.*\.log$/.test(f))
    .map((f) => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n');

const checkLines = (text) => {
    const m = new Map();
    for (const line of text.split('\n')) {
        const r = /ggpo: frame (\d+) board ([0-9a-f]{8})/.exec(line);
        if (r) m.set(Number(r[1]), r[2]);
    }
    return m;
};

/* Coin both cabinets up and start them into the race. */
async function driveToRace(hold, watch) {
    for (let k = 0; k < 2; k++) { await hold(COIN, 8); await hold(0, 8); }
    await hold(START, 8); await hold(0, 90); await watch();
    for (let k = 0; k < 6; k++) { await hold(START, 8); await hold(0, 120); await watch(); }
}

/* One machine's report: its work per frame, its twin's lines and its rows.
 * a and b are its frame samples at the race's start and end. */
function sideRows(c, i, { a, b, lob, text, r }, row) {
    const fps = (b.f - a.f) / ((b.t - a.t) / 1000);
    const e = b.emu;
    const twin = text.split('\n').filter((s) => /twin: /.test(s));
    const last = twin.filter((s) => /twin: frame/.test(s)).pop() ?? '';
    console.log(`${c.name} (${i ? 'SLAVE' : 'MASTER'}), ${lob.direct ? 'direct' : 'relayed'}: ` +
                `work ${e ? (e.work_us / Math.max(1, e.frames) / 1000).toFixed(2) : '?'} ms/frame`);
    for (const s of twin.filter((s) => !/twin: frame/.test(s)).concat(last ? [last] : []))
        console.log('    ' + s.replace(/^.*?twin: /, 'twin: '));
    row('no NETWORK ERROR', r.errors === 0, `_SubMode 9 in ${r.errors} of ${r.samples} samples`);
    row('race clock ran', r.first !== null && r.last - r.first >= (SECONDS - 5) * 60,
        `clock ${r.first} -> ${r.last}`);
    row('both cabinets', twin.some((s) => /both cabinets on this machine/.test(s)), 'twin started');
    row('link up', /A up .*B up/.test(last) && / 0 dropped/.test(last), last.replace(/^.*?twin: /, '') || 'no twin line');
    row('60 Hz', Math.abs(fps - 60) < 2.0, fps.toFixed(2) + ' frames/s');
}

async function main() {
    const tag = Date.now().toString(36).slice(-5);
    const [ua, ub] = ['raidA' + tag, 'raidB' + tag];
    const clients = [];
    const stop = async () => { for (const c of clients) await c.close().catch(() => {}); };
    process.on('SIGINT', () => stop().then(() => process.exit(130)));
    try {
        /* The one who accepts first, in the lobby before the challenge. */
        const p2 = await launch('p2', ub, ua, false);
        clients.push(p2);
        await waitFor(ub + ' in the lobby', async () => /channel|signed/.test(JSON.stringify(await lobby(p2))), 60000);
        const p1 = await launch('p1', ua, ub, true);
        clients.unshift(p1);
        const main = p1;
        await waitFor('GGPO running on both', async () =>
            (await lobby(main)).session?.running && (await lobby(p2)).session?.running, 180000);
        const l = await lobby(main);
        console.log(`${ua} (MASTER) vs ${ub} (SLAVE): GGPO running; lobby ${JSON.stringify({ stage: l.stage, direct: l.direct })}`);
        const hold = async (mask, frames) => {
            const f0 = await sessionFrames(main);
            for (const c of clients) await c.setInput(mask);
            await waitFor('a press', async () => (await sessionFrames(main)) >= f0 + frames);
        };
        await waitFor('the linked attract', async () => (await sessionFrames(main)) > 2300, 180000);
        const race = clients.map(() => ({ errors: 0, samples: 0, first: null, last: null }));
        const watch = async () => {
            for (const [i, c] of clients.entries()) {
                const l = await link(c);
                race[i].samples++;
                if (l.sub === 9) race[i].errors++;
                if (l.clock) { race[i].first ??= l.clock; race[i].last = l.clock; }
            }
        };
        await driveToRace(hold, watch);
        for (const r of race) r.first = null;
        const a = await Promise.all(clients.map(async (c) => ({ t: Date.now(), f: await sessionFrames(c) })));
        for (const c of clients) await c.setInput(THROTTLE);
        const t0 = Date.now();
        while (Date.now() - t0 < SECONDS * 1000) {
            await watch();
            await sleep(500);
        }
        for (const c of clients) await c.setInput(0);
        const b = await Promise.all(clients.map(async (c) => {
            const st = await c.status();
            return { t: Date.now(), f: await sessionFrames(c), emu: st.emu };
        }));
        const lob = await Promise.all(clients.map(lobby));
        await stop();
        let ok = true;
        const row = (what, pass, val) => { console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${what}: ${val}`); ok &&= pass; };
        const texts = clients.map((c) => logText(c.dir));
        for (const [i, c] of clients.entries())
            sideRows(c, i, { a: a[i], b: b[i], lob: lob[i], text: texts[i], r: race[i] }, row);
        const [l1, l2] = texts.map(checkLines);
        const both = [...l1.keys()].filter((f) => l2.has(f));
        const split = both.filter((f) => l1.get(f) !== l2.get(f));
        row('board checks agree', both.length > 0 && split.length === 0,
            `${both.length} frames compared${split.length ? ', split at ' + split[0] : ''}`);
        console.log(ok ? 'PASS' : 'FAIL');
        process.exitCode = ok ? 0 : 1;
    } finally {
        await stop();
    }
}

main().catch((e) => { console.error(e.stack ?? e); process.exitCode = 2; });
