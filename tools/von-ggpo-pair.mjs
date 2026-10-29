#!/usr/bin/env node
/*
 * von-ggpo-pair.mjs — a GGPO match of Virtual-On between two emulators on this
 * machine (Pinboard #1080, core/emu_twin.h).
 *
 * Two headless m2hle, straight to each other over UDP on 127.0.0.1 (no
 * lobby, no server). Each runs both cabinets of the link; player 1 sees board
 * A (the MASTER), player 2 board B (the SLAVE). The script puts a coin in and
 * presses Start on each player's cabinet, holds a lever, and checks:
 *   - each live board's link signs: role byte 1 / 0, link_ID 1 / 2, net_flag 1;
 *   - both cabinets reach the game (MainMode 4) as a versus match;
 *   - the two machines' "ggpo: frame N board X" lines agree, every one;
 *   - about 60 frames a second of wall time on each.
 *
 *   node tools/von-ggpo-pair.mjs --exe build/m2hle --rom $ROMS_DIR/vonj.zip \
 *        [--seconds 60] [--delay 2] [--dir /tmp/run]
 *
 * Exit 0 if every check holds.
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
/* The profile is the set's (vonj Japan, von USA; profiles/vonj.h). */
const PROFILE = path.basename(ROM, '.zip') === 'von' ? 'von' : 'vonj';
const SECONDS = Number(opt('seconds', '60'));
const DELAY   = opt('delay', '2');
const DIR     = opt('dir', null) ?? fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'von-ggpo-'));

/* A free port from the OS: TCP's, and the UDP one of the same number is free
 * as good as always on a quiet loopback. */
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

/* profiles/vonj.h's addresses, the same in von. */
async function signs(c) {
    const r = await c.rpc('read_many', { ranges: [['0x01D00028', 1], ['0x005770B0', 2], ['0x005039F4', 4],
                                                   ['0x00503A7C', 4], ['0x00503A98', 4], ['0x00503A9C', 4]] });
    const d = r.data.map((x) => (typeof x === 'string' ? x : x.data ?? x.hex));
    return { role_byte: le(d[0], 1), net_flag: le(d[1], 1), link_id: le(d[1].slice(2), 1),
             main_mode: le(d[2], 4), versus: le(d[3], 4), machine_p: le(d[4], 4), machine_e: le(d[5], 4) };
}

async function launch(name, player, local, remote, delay) {
    const dir = path.join(DIR, name);
    fs.mkdirSync(dir, { recursive: true });
    const port = await freePort();
    const c = await M2Hle.launch({
        rom: ROM, port, exe: EXE, profile: PROFILE, region: null, cwd: dir,
        extraArgs: ['--no-nvram', '--net-config', path.join(dir, 'netplay.cfg'),
                    '--ggpo-local', String(local), '--ggpo-remote', `127.0.0.1:${remote}`,
                    '--ggpo-player', String(player), '--ggpo-delay', delay],
    });
    c.name = name;
    c.dir = dir;
    return c;
}

/* m2hle writes m2hle-<pid>.log in its working directory. */
const checkLines = (dir) => {
    const m = new Map();
    const text = fs.readdirSync(dir).filter((f) => /^m2hle-\d+\.log$/.test(f))
        .map((f) => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n');
    for (const line of text.split('\n')) {
        const r = /ggpo: frame (\d+) board ([0-9a-f]{8})/.exec(line);
        if (r) m.set(Number(r[1]), r[2]);
    }
    return m;
};

/* A press held for `frames` board frames, then let go. */
async function press(c, bits, frames = 6) {
    await c.setInput(bits);
    await c.waitFrames(frames).catch(() => {});
    await c.setInput(0);
    await c.waitFrames(frames).catch(() => {});
}

async function main() {
    const [u1, u2] = [await freePort(), await freePort()];
    const clients = [];
    const stop = async () => { for (const c of clients) await c.close().catch(() => {}); };
    process.on('SIGINT', () => stop().then(() => process.exit(130)));
    try {
        const p1 = await launch('p1', 1, u1, u2, DELAY); clients.push(p1);
        const p2 = await launch('p2', 2, u2, u1, DELAY); clients.push(p2);
        await Promise.all(clients.map((c) => c.waitUntilRunning({ timeoutMs: 120000 })));
        console.log(`p1 on UDP ${u1}, p2 on UDP ${u2}; running`);
        /* Past the boot and the link check to the attract. Not by frame count:
         * the session restarts it after the twin's cold boot. */
        for (const c of clients) {
            const until = Date.now() + 120000;
            while ((await signs(c)).main_mode !== 1) {
                if (Date.now() > until) throw new Error(`${c.name}: never reached the attract`);
                await sleep(500);
            }
        }
        /* A coin in both at once: one coined while the other already plays is
         * a challenge, later. Then Shot (IN1 0x01) now and then takes the
         * select screens through to the round. */
        for (const c of clients) await c.setInput(0x01);
        await sleep(300);
        for (const c of clients) await c.setInput(0);
        for (let t = 0; t < 4; t++) {
            await sleep(3000);
            for (const c of clients) await c.setInput(0x100);
            await sleep(200);
            for (const c of clients) await c.setInput(0);
        }
        const a = await Promise.all(clients.map(async (c) => ({ t: Date.now(), f: (await c.status()).frames })));
        const deadline = Date.now() + SECONDS * 1000;
        let k = 0;
        while (Date.now() < deadline) {   /* lever pushes now and then, each player their own */
            const c = clients[k++ % 2];
            await press(c, k % 3 ? 0x00004000 : 0x00000100, 20);
            await sleep(500);
        }
        const b = await Promise.all(clients.map(async (c) => {
            const st = await c.status();
            return { t: Date.now(), f: st.frames, emu: st.emu };
        }));
        const s = await Promise.all(clients.map(signs));
        const want = [{ role_byte: 1, link_id: 1 }, { role_byte: 0, link_id: 2 }];
        let ok = true;
        const row = (what, pass, val) => { console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${what}: ${val}`); ok &&= pass; };
        for (const [i, c] of clients.entries()) {
            const fps = (b[i].f - a[i].f) / ((b[i].t - a[i].t) / 1000);
            const e = b[i].emu;
            console.log(`${c.name}: ${JSON.stringify(s[i])}; work ${e ? (e.work_us / Math.max(1, e.frames) / 1000).toFixed(2) : '?'} ms/frame`);
            row('role byte', s[i].role_byte === want[i].role_byte, s[i].role_byte);
            row('link_ID', s[i].link_id === want[i].link_id, s[i].link_id);
            row('net_flag', s[i].net_flag === 1, s[i].net_flag);
            row('in the game', s[i].main_mode === 4, `MainMode ${s[i].main_mode}`);
            row('against the other cabinet', s[i].versus === 1, `VersusMode ${s[i].versus}`);
            row('60 Hz', Math.abs(fps - 60) < 2.0, fps.toFixed(2) + ' frames/s');
        }
        row('each the other\'s opponent', s[0].machine_e === s[1].machine_p && s[1].machine_e === s[0].machine_p,
            `p1 ${s[0].machine_p} vs ${s[0].machine_e}, p2 ${s[1].machine_p} vs ${s[1].machine_e}`);
        await stop();
        const [l1, l2] = clients.map((c) => checkLines(c.dir));
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
