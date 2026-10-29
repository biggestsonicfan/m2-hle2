#!/usr/bin/env node
/*
 * powsled-ggpo-pair.mjs — a GGPO match of Power Sled between two emulators on
 * this machine, set up by a local ggpo-server lobby (Pinboard #1095;
 * core/emu_twin.h, profiles/powsled.h).
 *
 * Each emulator runs both boards of the link: board A the Main unit (player
 * 1, the challenger), board B a ride (player 2, who accepts). Only the two
 * players' inputs cross the network. The two sign up on the lobby, one
 * challenges the other, and once GGPO runs the script plays the start
 * sequence of powsled-rpcn-pair.mjs on GGPO's frame count: the Main calls
 * (P1 B2), the ride enters (P1 B1), the Main steps the panel (P2 B2 twice) and
 * starts the race (P2 B3); then the ride holds a pedal. It checks:
 *   - the race state (0x53E744, 7 = racing) reaches 7 on each live board;
 *   - the two machines' "ggpo: frame N board X" lines agree, every one;
 *   - both ends of the twin's link are up and nothing was dropped;
 *   - about 60 frames a second of wall time on each.
 *
 * The lobby must be a local ggpo-server, never the live one:
 *   GGPO_HTTP_PORT=<p> GGPO_UDP_PORT=<u> GGPO_PUBLIC_HOST=127.0.0.1 \
 *       GGPO_DB=/tmp/x/ggpo.db node <ggpo-server>/src/server.mjs
 *   node tools/powsled-ggpo-pair.mjs --exe build/m2hle --rom $ROMS_DIR/powsled.zip \
 *        --lobby ws://127.0.0.1:<p>/ws [--seconds 30] [--delay 2] [--relay] [--dir /tmp/run]
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
const ROM     = opt('rom', path.join(process.env.ROMS_DIR ?? '', 'powsled.zip'));
const LOBBY   = opt('lobby', null);
const SECONDS = Number(opt('seconds', '30'));
const DELAY   = opt('delay', '2');
const RELAY   = args.includes('--relay');
const SYNCLOG = args.includes('--synclog');   /* each check frame's parts, in <dir>/<p>/sync */
const EXTRA   = (opt('args', '') || '').split(' ').filter(Boolean);   /* more m2hle flags, both */
const DIR     = opt('dir', null) ?? fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'powsled-ggpo-'));

if (!LOBBY || !/^ws:\/\/(127\.0\.0\.1|localhost)[:/]/.test(LOBBY)) {
    console.error('powsled-ggpo-pair: --lobby ws://127.0.0.1:PORT/ws, a local ggpo-server only');
    process.exit(2);
}

const freePort = () => new Promise((res, rej) => {
    const s = net.createServer();
    s.once('error', rej);
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
});

/* The race state: 05 network check, 06 entry, 07 racing. */
async function raceState(c) {
    const r = await c.rpc('read_many', { ranges: [['0x0053E744', 1]] });
    const d = r.data.map((x) => (typeof x === 'string' ? x : x.data ?? x.hex));
    return parseInt(d[0].substr(0, 2), 16);
}

const ENTRY = 0x100, CALL = 0x200, STEP = 0x800, GO = 0x20000, PEDAL = 0x8;

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

/* A press held for `frames` of GGPO's frames, then let go. */
async function press(c, mask, frames = 10) {
    const f0 = await sessionFrames(c);
    await c.setInput(mask);
    await waitFor('a press', async () => (await sessionFrames(c)) >= f0 + frames);
    await c.setInput(0);
}

async function launch(name, user, other, challenger) {
    const dir = path.join(DIR, name);
    fs.mkdirSync(dir, { recursive: true });
    const port = await freePort();
    const c = await M2Hle.launch({
        rom: ROM, port, exe: EXE, profile: 'powsled', region: null, cwd: dir,
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

async function main() {
    const tag = Date.now().toString(36).slice(-5);
    const [ua, ub] = ['sledA' + tag, 'sledB' + tag];
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
        const [main, ride] = [p1, p2];
        await waitFor('GGPO running on both', async () =>
            (await lobby(main)).session?.running && (await lobby(ride)).session?.running, 180000);
        const l = await lobby(main);
        console.log(`${ua} (Main) vs ${ub} (ride): GGPO running; lobby ${JSON.stringify({ stage: l.stage, direct: l.direct })}`);
        /* The frames of #1089's room test, counted from the session's cold boot. */
        for (const [c, f, mask] of [[main, 2400, CALL], [ride, 2700, ENTRY], [main, 3000, STEP],
                                    [main, 3200, STEP], [main, 3600, GO]]) {
            await waitFor('frame ' + f, async () => (await sessionFrames(c)) >= f, 180000);
            await press(c, mask);
            console.log(`  [${c.name}] frame ${f}: 0x${mask.toString(16)}; race states ${await raceState(main)}, ${await raceState(ride)}`);
        }
        const a = await Promise.all(clients.map(async (c) => ({ t: Date.now(), f: await sessionFrames(c) })));
        const peak = [0, 0];
        await ride.setInput(PEDAL);
        const t0 = Date.now();
        while (Date.now() - t0 < SECONDS * 1000) {
            for (const [i, c] of clients.entries()) peak[i] = Math.max(peak[i], await raceState(c));
            await sleep(500);
        }
        await ride.setInput(0);
        const b = await Promise.all(clients.map(async (c) => {
            const st = await c.status();
            return { t: Date.now(), f: await sessionFrames(c), emu: st.emu };
        }));
        const now = await Promise.all(clients.map(raceState));
        const lob = await Promise.all(clients.map(lobby));
        await stop();
        let ok = true;
        const row = (what, pass, val) => { console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${what}: ${val}`); ok &&= pass; };
        const texts = clients.map((c) => logText(c.dir));
        for (const [i, c] of clients.entries()) {
            const fps = (b[i].f - a[i].f) / ((b[i].t - a[i].t) / 1000);
            const e = b[i].emu;
            const twin = texts[i].split('\n').filter((s) => /twin: /.test(s));
            const last = twin.filter((s) => /twin: frame/.test(s)).pop() ?? '';
            console.log(`${c.name} (${i ? 'ride' : 'Main'}), ${lob[i].direct ? 'direct' : 'relayed'}: ` +
                        `work ${e ? (e.work_us / Math.max(1, e.frames) / 1000).toFixed(2) : '?'} ms/frame`);
            for (const s of twin.filter((s) => !/twin: frame/.test(s)).concat(last ? [last] : []))
                console.log('    ' + s.replace(/^.*?twin: /, 'twin: '));
            row('raced', peak[i] === 7, `race state peaked at ${peak[i]}, now ${now[i]}`);
            row('both cabinets', twin.some((s) => /both cabinets on this machine/.test(s)), 'twin started');
            row('link up', /A up .*B up/.test(last) && / 0 dropped/.test(last), last.replace(/^.*?twin: /, '') || 'no twin line');
            row('60 Hz', Math.abs(fps - 60) < 2.0, fps.toFixed(2) + ' frames/s');
        }
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
