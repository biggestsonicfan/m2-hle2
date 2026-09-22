/*
 * bench-builds.mjs — how fast does each build run the board, headless?
 *
 * The companion of ab-builds.mjs: that one asks whether two builds compute the
 * same board, this one asks how quickly. Each build boots the ROM unthrottled
 * (M2HLE_UNTHROTTLE=1, no window, no audio device), runs past the texture-load
 * spike, and is then timed over a window of game frames read off get_status.
 * The builds alternate, so a background load on the host hits both alike, and
 * the best of each build's rounds is reported — the least-disturbed run is the
 * one closest to what the code costs.
 *
 *   node tools/bench-builds.mjs <exeA> <exeB> [--rounds 3] [--skip 400] [--seconds 10] [--rom <zip>]
 */
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['rounds', 'skip', 'seconds', 'rom', 'port']);
const exes = process.argv.slice(2).filter((a) => !a.startsWith('--') && /\.exe$|m2hle$/i.test(a));
if (exes.length < 1) { console.error('usage: node tools/bench-builds.mjs <exeA> [exeB ...] [--rounds 3]'); process.exit(2); }
const ROUNDS  = args.num('rounds', 3);
const SKIP    = args.num('skip', 400);
const SECONDS = args.num('seconds', 10);
const ROM     = path.resolve(args.str('rom', 'C:/Users/bigge/source/repos/ai/m2-hle2/test_m2snake/sfight.zip'));
const PORT    = args.num('port', 7321);
process.env.M2HLE_UNTHROTTLE = '1';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function measure(exe, tag, port) {
    const dir = path.join(os.tmpdir(), 'm2hle-bench-' + tag);
    fs.mkdirSync(dir, { recursive: true });
    for (const z of ['sfight.zip', 'schamp.zip']) {
        const src = path.join(path.dirname(ROM), z);
        if (fs.existsSync(src)) fs.copyFileSync(src, path.join(dir, z));
    }
    const emu = await M2Hle.launch({ exe, rom: path.join(dir, 'sfight.zip'), port, run: true });
    try {
        await emu.waitForRom();
        for (;;) {
            const s = await emu.rpc('get_status');
            if (s.frames >= SKIP) break;
            await sleep(100);
        }
        const s0 = await emu.rpc('get_status');
        const t0 = process.hrtime.bigint();
        await sleep(SECONDS * 1000);
        const s1 = await emu.rpc('get_status');
        const t1 = process.hrtime.bigint();
        return (s1.frames - s0.frames) / (Number(t1 - t0) / 1e9);
    } finally { await emu.close(); }
}

const best = exes.map(() => 0), all = exes.map(() => []);
for (let r = 0; r < ROUNDS; r++) {
    for (let i = 0; i < exes.length; i++) {
        const fps = await measure(exes[i], String.fromCharCode(97 + i), PORT + i);
        all[i].push(fps);
        if (fps > best[i]) best[i] = fps;
        console.log(`round ${r + 1} ${path.basename(path.dirname(path.dirname(exes[i])))}: ${fps.toFixed(1)} fps`);
    }
}
for (let i = 0; i < exes.length; i++)
    console.log(`${exes[i]}\n  best ${best[i].toFixed(1)} fps  (rounds: ${all[i].map((f) => f.toFixed(1)).join(', ')})`);
if (exes.length >= 2)
    console.log(`B/A: ${(100 * best[1] / best[0] - 100).toFixed(1)}%`);
