#!/usr/bin/env node
/*
 * lan-ggpo-pair.mjs — a direct GGPO match, no lobby (Pinboard #1891,
 * net/ggpo_lan.h), between two emulators on this machine.
 *
 * One hosts (--lan-host PORT, player 1), the other joins it at 127.0.0.1:PORT
 * over UDP, or over the host's WebSocket door with --ws (ws://127.0.0.1:PORT,
 * the way a browser joins). It checks:
 *   - both reach a running GGPO session, each on its side ("1P" / "2P");
 *   - the two machines' "ggpo: frame N board X" lines agree, every one;
 *   - a third machine that joins while they play is refused;
 *   - with --rematch: the host's "end" puts it back to waiting, the joiner
 *     joins again and the second match agrees too.
 *
 *   node tools/lan-ggpo-pair.mjs --exe build/m2hle --rom $ROMS_DIR/sfight.zip \
 *        [--seconds 20] [--ws] [--rematch] [--synclog] [--via 100.x.y.z] [--dir /tmp/run]
 *
 * Every port is a free one. Exit 0 if every check holds.
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
const ROM     = opt('rom', path.join(process.env.ROMS_DIR ?? '', 'sfight.zip'));
const SECONDS = Number(opt('seconds', '20'));
const WS      = args.includes('--ws');
const REMATCH = args.includes('--rematch');
const VIA     = opt('via', '127.0.0.1');      /* the host's address to join: a LAN or tailnet one */
const SYNCLOG = args.includes('--synclog');   /* each check frame's parts, in <dir>/<name>/sync */
const DIR     = opt('dir', null) ?? fs.mkdtempSync(path.join(process.env.TMPDIR ?? os.tmpdir(), 'lan-ggpo-'));

const freePort = () => new Promise((res, rej) => {
    const s = net.createServer();
    s.once('error', rej);
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
});

const lobby = (c) => c.rpc('ggpo_lobby', {});
const step = (c, what, extra = {}) => c.rpc('ggpo_lobby', { do: what, ...extra });

async function waitFor(what, fn, ms = 60000) {
    const deadline = Date.now() + ms;
    for (;;) {
        const v = await fn();
        if (v) return v;
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + what);
        await sleep(250);
    }
}

async function launch(name, extra) {
    const dir = path.join(DIR, name);
    fs.mkdirSync(dir, { recursive: true });
    if (SYNCLOG) { fs.mkdirSync(path.join(dir, 'sync'), { recursive: true }); extra = [...extra, '--ggpo-synclog', path.join(dir, 'sync')]; }
    const c = await M2Hle.launch({
        rom: ROM, port: await freePort(), exe: EXE, cwd: dir,
        extraArgs: ['--no-nvram', '--net-config', path.join(dir, 'netplay.cfg'), ...extra],
    });
    c.name = name;
    c.dir = dir;
    return c;
}

const logText = (dir) => fs.readdirSync(dir).filter((f) => /^m2hle.*\.log$/.test(f))
    .map((f) => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n');

/* The check lines of each match, in order: a match starts at "lan: match". */
const matches = (text) => {
    const out = [];
    for (const line of text.split('\n')) {
        if (/lan: match against/.test(line)) out.push(new Map());
        const r = /ggpo: frame (\d+) board ([0-9a-f]{8})/.exec(line);
        if (r && out.length) out[out.length - 1].set(Number(r[1]), r[2]);
    }
    return out;
};

async function play(clients, label, row) {
    await waitFor(label + ': GGPO running on both', async () =>
        (await Promise.all(clients.map(lobby))).every((l) => l.session?.running), 120000);
    const l = await Promise.all(clients.map(lobby));
    row(label + ': sides', l[0].user === '1P' && l[1].user === '2P' && l.every((x) => x.lan && x.match === 'lan'),
        l.map((x) => `${x.user} vs ${x.opponent}`).join(', '));
    /* Coins and Start on both, then let it run. */
    for (const mask of [0x1, 0, 0x1, 0, 0x40, 0]) {
        for (const c of clients) await c.setInput(mask);
        await sleep(300);
    }
    await sleep(SECONDS * 1000);
    const f = await Promise.all(clients.map(async (c) => (await lobby(c)).session?.frames ?? 0));
    row(label + ': frames ran', f.every((n) => n > SECONDS * 40), f.join(' / '));
}

async function main() {
    const clients = [];
    const stop = async () => { for (const c of clients) await c.close().catch(() => {}); };
    process.on('SIGINT', () => stop().then(() => process.exit(130)));
    let ok = true;
    const row = (what, pass, val) => { console.log(`  ${pass ? 'ok  ' : 'FAIL'} ${what}: ${val}`); ok &&= pass; };
    try {
        const lanPort = await freePort();
        const addr = (WS ? 'ws://' : '') + VIA + ':' + lanPort;
        const host = await launch('host', ['--lan-host', String(lanPort)]);
        clients.push(host);
        const h = await waitFor('the host waiting', async () => { const l = await lobby(host); return l.stage === 'lan_host' && l; });
        console.log(`host on ${lanPort}: ${h.url}`);
        const join = await launch('join', ['--lan-join', addr]);
        clients.push(join);
        await play(clients, 'match 1', row);

        /* A third player while they play: the host says no. */
        const third = await launch('third', ['--lan-join', addr]);
        const t = await waitFor('the third refused', async () => { const l = await lobby(third); return l.stage === 'failed' && l; });
        row('third refused', /already playing/.test(t.error), t.error);
        await third.close().catch(() => {});

        if (REMATCH) {
            await step(host, 'end');
            await waitFor('the host waiting again', async () => (await lobby(host)).stage === 'lan_host', 30000);
            await waitFor('the joiner done', async () => (await lobby(join)).stage === 'off', 30000);
            await waitFor('both sessions closed', async () =>
                (await Promise.all(clients.map(lobby))).every((l) => !l.session?.on), 30000);
            await step(join, 'lan_join', { addr });
            await play(clients, 'match 2', row);
        }
        await stop();
        const per = clients.map((c) => matches(logText(c.dir)));
        for (let m = 0; m < Math.min(per[0].length, per[1].length); m++) {
            const [l1, l2] = [per[0][m], per[1][m]];
            const both = [...l1.keys()].filter((f) => l2.has(f));
            const split = both.filter((f) => l1.get(f) !== l2.get(f));
            row(`match ${m + 1}: board checks agree`, both.length > 0 && split.length === 0,
                `${both.length} frames compared${split.length ? ', split at ' + split[0] : ''}`);
        }
        row('matches logged', per[0].length === (REMATCH ? 2 : 1) && per[1].length === per[0].length,
            per.map((p) => p.length).join(' / '));
        console.log(ok ? 'PASS' : 'FAIL');
        process.exitCode = ok ? 0 : 1;
    } finally {
        await stop();
    }
}

main().catch((e) => { console.error(e.stack ?? e); process.exitCode = 2; });
