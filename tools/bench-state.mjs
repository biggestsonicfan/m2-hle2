/*
 * bench-state.mjs — how fast does a build run ONE game state?
 *
 * bench-builds.mjs times the board in steady flight, deliberately past the
 * texture-load spike. That is the wrong window for the states that hitch: the
 * VS screen (ROUND_MASK) runs fifty times an ordinary frame's instructions and
 * is over in a second, so a ten-second average hides it completely.
 *
 * This times a window instead. Each build is driven to a breakpoint at the
 * state's entry (`--from`) and the clock runs from there to a breakpoint at
 * whatever the ROM hands over to (`--to`).
 *
 * The number reported is INSTRUCTIONS A SECOND, not milliseconds, because the
 * board free-runs into the state (lib/drive.mjs says why it must) and so enters
 * it from a frame that wanders by a few either way: two runs of the same build
 * differ by ±15% of wall clock and by a fraction of a percent of throughput.
 * The window's own instruction count is printed beside it, so the milliseconds
 * an optimisation is worth are the last line of the report.
 *
 *   node tools/bench-state.mjs <exeA> <exeB> --state round-mask --rounds 3
 *   node tools/bench-state.mjs <exeA> <exeB> --from 0xB820 --to 0xC34C
 *
 * Both builds need a get_status that reports "steps" (from the commit that
 * added this tool); an older build is refused rather than silently mismeasured.
 */
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { parseArgs } from './lib/args.mjs';
import { driveToAddr, fightInputAt, attractInputAt } from './lib/drive.mjs';

/* Entry and hand-over addresses, read off the ROM's own state tables
 * (ROUND_STATE for a round, the advertise table for attract). */
const STATES = {
    'round-mask': { from: 0xB820, to: 0xC34C, drive: 'fight',
                    what: "ROUND_MASK_INT -> ROUND_INT: the VS screen, where the round's textures load" },
};

const args = parseArgs(['from', 'to', 'state', 'rounds', 'rom', 'port', 'drive', 'max-frames']);
const exes = process.argv.slice(2).filter((a) => !a.startsWith('--') && /\.exe$|m2hle$/i.test(a))
    .map((a) => path.resolve(a));
if (!exes.length) { console.error('usage: node tools/bench-state.mjs <exeA> [exeB ...] [--state round-mask]'); process.exit(2); }

const named  = STATES[args.str('state', 'round-mask')] ?? {};
const FROM   = args.str('from') ? Number(args.str('from')) : named.from;
const TO     = args.str('to')   ? Number(args.str('to'))   : named.to;
const DRIVE  = args.str('drive', named.drive ?? 'fight');
const ROUNDS = args.num('rounds', 3);
const ROM    = path.resolve(args.str('rom', 'C:/Users/bigge/source/repos/ai/m2-hle2/test_m2snake/sfight.zip'));
const PORT   = args.num('port', 7351);
const MAXF   = args.num('max-frames', 12000);
if (FROM === undefined || TO === undefined) { console.error('need --from and --to (or a known --state)'); process.exit(2); }

process.env.M2HLE_UNTHROTTLE = '1';
const hex = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(8, '0');

/* Each build in its own directory: neither shares m2hle.log with the other or
 * with an emulator someone is already running. */
function rundir(tag) {
    const dir = path.join(os.tmpdir(), 'm2hle-state-' + tag);
    fs.mkdirSync(dir, { recursive: true });
    for (const z of ['sfight.zip', 'schamp.zip']) {
        const src = path.join(path.dirname(ROM), z);
        if (fs.existsSync(src)) fs.copyFileSync(src, path.join(dir, z));
    }
    return path.join(dir, path.basename(ROM));
}

async function measure(exe, tag, port) {
    const emu = await M2Hle.launch({ exe, rom: rundir(tag), port, run: true });
    try {
        await emu.waitForRom();
        const at = await driveToAddr(emu, FROM, {
            inputAt: DRIVE === 'fight' ? fightInputAt : attractInputAt, maxFrames: MAXF,
        });
        await emu.clearAllBreakpoints();
        await emu.setBreakpoint(TO, 'to');
        const s0 = await emu.status();
        if (s0.steps === undefined) throw new Error(`${exe}: this build's get_status has no "steps"`);
        const t0 = process.hrtime.bigint();
        await emu.run();
        await emu.waitForStop(300000);
        const t1 = process.hrtime.bigint();
        const s1 = await emu.status();
        if (Number(s1.ip) !== TO) throw new Error(`the window ended at ${s1.ip}, not ${hex(TO)}`);
        const ms = Number(t1 - t0) / 1e6;
        const steps = Number(s1.steps) - Number(s0.steps);
        return { ms, steps, mips: steps / (ms * 1000), at };
    } finally { await emu.close(); }
}

const all = exes.map(() => []);
for (let r = 0; r < ROUNDS; r++) {
    for (let i = 0; i < exes.length; i++) {
        const tag = String.fromCharCode(97 + i);
        const m = await measure(exes[i], tag, PORT + i);
        all[i].push(m);
        console.log(`round ${r + 1} ${tag}: ${m.mips.toFixed(1)} Mi/s ` +
                    `(${m.steps.toLocaleString()} instructions in ${m.ms.toFixed(1)} ms, entered at frame ~${m.at})`);
    }
}

console.log(`\nwindow ${hex(FROM)} -> ${hex(TO)}${named.what ? '   ' + named.what : ''}`);
const best = all.map((v) => Math.max(...v.map((m) => m.mips)));
const work = all[0].reduce((a, m) => a + m.steps, 0) / all[0].length;
exes.forEach((e, i) => {
    console.log(`  ${String.fromCharCode(97 + i)}  ${e}`);
    console.log(`     best ${best[i].toFixed(1)} Mi/s   (${all[i].map((m) => m.mips.toFixed(1)).join(', ')})`);
});
for (let i = 1; i < exes.length; i++) {
    const gain = 100 * (best[i] - best[0]) / best[0];
    console.log(`  ${String.fromCharCode(97 + i)} vs a: ${gain >= 0 ? '+' : ''}${gain.toFixed(1)}%   ` +
                `the state's ${(work / 1e6).toFixed(2)}M instructions: ` +
                `${(work / best[0] / 1000).toFixed(0)} ms -> ${(work / best[i] / 1000).toFixed(0)} ms`);
}
