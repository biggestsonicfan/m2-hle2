/*
 * bench-render.mjs — what does the host pay to draw a frame, stage by stage?
 *
 * Each build runs headless with the A/V server up and a client draining it, so
 * the main thread renders every board frame on the real D3D11 device exactly
 * as a window would, paced at 60 Hz. get_status carries game_frame.h's stage
 * timers; two readings some seconds apart give the microseconds each stage
 * costs per rendered frame. Builds alternate, the best (least disturbed)
 * round of each is reported.
 *
 *   node tools/bench-render.mjs <exeA> [exeB] [--rounds 3] [--skip 500] [--seconds 10] [--rom <zip>]
 */
import os from 'node:os';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['rounds', 'skip', 'seconds', 'rom', 'port']);
const exes = process.argv.slice(2).filter((a) => !a.startsWith('--') && /\.exe$|m2hle$/i.test(a));
if (exes.length < 1) { console.error('usage: node tools/bench-render.mjs <exeA> [exeB ...]'); process.exit(2); }
const ROUNDS  = args.num('rounds', 3);
const SKIP    = args.num('skip', 500);
const SECONDS = args.num('seconds', 10);
const ROM     = path.resolve(args.str('rom', 'C:/Users/bigge/source/repos/ai/m2-hle2/test_m2snake/sfight.zip'));
const PORT    = args.num('port', 7361);
const STAGES  = ['compose_us', 'scan_us', 'upload_us', 'draw3d_us', 'tiles_us'];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* A client that reads and discards: the headless loop renders only while one is attached. */
function drain(port) {
    const sock = net.connect(port, '127.0.0.1');
    sock.on('data', () => {});
    sock.on('error', () => {});
    return sock;
}

async function measure(exe, tag, port) {
    const dir = path.join(os.tmpdir(), 'm2hle-render-' + tag);
    fs.mkdirSync(dir, { recursive: true });
    for (const z of ['sfight.zip', 'schamp.zip']) {
        const src = path.join(path.dirname(ROM), z);
        if (fs.existsSync(src)) fs.copyFileSync(src, path.join(dir, z));
    }
    const avPort = port + 100;
    const emu = await M2Hle.launch({ exe, rom: path.join(dir, 'sfight.zip'), port, run: true,
                                     extraArgs: ['--av-port', String(avPort)] });
    let sock = null;
    try {
        await emu.waitForRom();
        sock = drain(avPort);
        for (;;) {
            const s = await emu.rpc('get_status');
            if (s.frames >= SKIP && s.render.frames > 30) break;
            await sleep(100);
        }
        const s0 = await emu.rpc('get_status');
        await sleep(SECONDS * 1000);
        const s1 = await emu.rpc('get_status');
        const n = s1.render.frames - s0.render.frames;
        const out = { frames: n, board: s1.frames - s0.frames, total: 0 };
        for (const k of STAGES) { out[k] = (s1.render[k] - s0.render[k]) / n; out.total += out[k]; }
        return out;
    } finally { if (sock) sock.destroy(); await emu.close(); }
}

const fmt = (r) => `${r.frames} frames (${r.board} board): total ${r.total.toFixed(0)} us/frame  ` +
    STAGES.map((k) => `${k.replace('_us', '')} ${r[k].toFixed(0)}`).join('  ');

const best = exes.map(() => null);
for (let r = 0; r < ROUNDS; r++) {
    for (let i = 0; i < exes.length; i++) {
        const res = await measure(exes[i], String.fromCharCode(97 + i), PORT + i * 2);
        if (!best[i] || res.total < best[i].total) best[i] = res;
        console.log(`round ${r + 1} ${path.basename(path.dirname(path.dirname(exes[i])))}: ${fmt(res)}`);
    }
}
for (let i = 0; i < exes.length; i++) console.log(`${exes[i]}\n  best: ${fmt(best[i])}`);
if (exes.length >= 2)
    for (const k of ['total', ...STAGES])
        console.log(`  ${k.padEnd(10)} A ${best[0][k].toFixed(0).padStart(6)}  B ${best[1][k].toFixed(0).padStart(6)}  ${(100 * best[1][k] / best[0][k] - 100).toFixed(1)}%`);
