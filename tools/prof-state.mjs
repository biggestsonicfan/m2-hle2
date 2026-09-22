/*
 * prof-state.mjs — what the BOARD spends its time on between two ROM addresses.
 *
 * A complaint about a game state ("the VS screen hitches") is a question about
 * ROM code, not about emulator code: the emulator is one interpreter loop and
 * runs every instruction at about the same price, so the state that costs is
 * the state that executes the most instructions, and which ones they are says
 * which subsystem is being asked for the work. bench-builds answers "how fast
 * does this build run the board"; this answers "and what is the board doing".
 *
 * It needs an instrumented emulator (cmake -DM2HLE_PROFILE=ON), which counts
 * i960 instructions per code address and the host microseconds of each frame.
 * The window is a pair of breakpoints: the profiler is armed when the first is
 * hit and dumped at the second.
 *
 *   node tools/prof-state.mjs --exe build_prof/Release/m2hle.exe \
 *        --from 0xB820 --to 0xC34C --out prof/round-mask
 *
 * --drive fight   insert a coin and mash through select until `from` is reached
 *                 (the default; --drive none just runs attract)
 * --ida PORT      symbolicate through the IDA bridge (default 7331; a bridge
 *                 that is not there is not an error, the addresses stay raw)
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { parseArgs } from './lib/args.mjs';
import { driveToAddr, fightInputAt } from './lib/drive.mjs';

const args = parseArgs(['exe', 'from', 'to', 'out', 'rom', 'port', 'drive', 'ida', 'max-frames', 'top']);
const EXE  = args.str('exe', null) ? path.resolve(args.str('exe')) : null;
const FROM = Number(args.str('from', '0xB820'));
const TO   = Number(args.str('to', '0xC34C'));
const OUT  = path.resolve(args.str('out', path.join(os.tmpdir(), 'm2hle-prof')));
const ROM  = path.resolve(args.str('rom', 'C:/Users/bigge/source/repos/ai/m2-hle2/test_m2snake/sfight.zip'));
const PORT = args.num('port', 7341);
const DRIVE = args.str('drive', 'fight');
const IDA  = args.num('ida', 7331);
const MAXF = args.num('max-frames', 12000);
const TOP  = args.num('top', 40);

process.env.M2HLE_UNTHROTTLE = '1';
fs.mkdirSync(OUT, { recursive: true });
const hex = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(8, '0');

/* ---- IDA symbols (optional) ---------------------------------------------- */
async function idaFunctions() {
    const out = new Map();
    try {
        for (let off = 0; off < 6000; off += 500) {
            const r = await fetch(`http://127.0.0.1:${IDA}/list_functions`, {
                method: 'POST', headers: { 'content-type': 'application/json' },
                body: JSON.stringify({ limit: 500, offset: off }),
                signal: AbortSignal.timeout(30000),
            });
            const j = await r.json();
            const fns = j.functions ?? j.results ?? [];
            if (!fns.length) break;
            for (const f of fns) out.set(Number(f.start ?? f.address), f.name);
        }
    } catch { /* no bridge: raw addresses are still a usable answer */ }
    return [...out.entries()].sort((a, b) => a[0] - b[0]);
}
function symbolizer(fns) {
    const at = fns.map(([a]) => a);
    return (addr) => {
        let lo = 0, hi = at.length - 1, best = -1;
        while (lo <= hi) { const m = (lo + hi) >> 1; if (at[m] <= addr) { best = m; lo = m + 1; } else hi = m - 1; }
        return best < 0 ? null : { name: fns[best][1], start: at[best] };
    };
}

/* ---- drive the board to the window --------------------------------------- */
const driveTo = (emu, addr) =>
    driveToAddr(emu, addr, { inputAt: DRIVE === 'fight' ? fightInputAt : () => 0, maxFrames: MAXF });

/* Its own directory, so it neither shares m2hle.log with a running emulator
 * nor writes into the ROM folder the stream reads from. */
const RUNDIR = path.join(OUT, 'run');
fs.mkdirSync(RUNDIR, { recursive: true });
for (const z of ['sfight.zip', 'schamp.zip']) {
    const src = path.join(path.dirname(ROM), z);
    if (fs.existsSync(src) && !fs.existsSync(path.join(RUNDIR, z))) fs.copyFileSync(src, path.join(RUNDIR, z));
}

const emu = await M2Hle.launch({ exe: EXE, rom: path.join(RUNDIR, path.basename(ROM)), port: PORT, run: true });
try {
    await emu.waitForRom();
    const probe = await emu.rpc('prof', { on: 0 });
    if (!probe.ok) throw new Error(probe.error ?? 'no profiler in this build');

    const reachedAt = await driveTo(emu, FROM);
    console.log(`${hex(FROM)} reached at frame ~${reachedAt}`);

    await emu.rpc('prof', { on: 1 });
    await emu.clearAllBreakpoints();
    await emu.setBreakpoint(TO, 'prof-to');
    await emu.run();
    await emu.waitForStop(180000);
    await emu.rpc('prof', { on: 0 });
    const dump = await emu.rpc('prof_dump', { path: path.join(OUT, 'pc.csv') });
    if (!dump.ok) throw new Error(dump.error);
} finally { await emu.close(); }

/* ---- report --------------------------------------------------------------- */
const rows = fs.readFileSync(path.join(OUT, 'pc.csv'), 'utf8').trim().split('\n').slice(1)
    .map((l) => l.split(',').map(Number));
const frames = fs.readFileSync(path.join(OUT, 'pc.csv.frames.csv'), 'utf8').trim().split('\n').slice(1)
    .filter((l) => l && !l.startsWith('#'))
    .map((l) => { const [f, us, st] = l.split(',').map(Number); return { f, us, st }; });

const sym = symbolizer(await idaFunctions());
const byFn = new Map();
for (const [addr, count] of rows) {
    const s = sym(addr);
    const key = s ? s.name : hex(addr & ~0xFF);
    const e = byFn.get(key) ?? { count: 0, start: s ? s.start : addr };
    e.count += count;
    byFn.set(key, e);
}
const total = rows.reduce((a, r) => a + r[1], 0);
const ranked = [...byFn.entries()].sort((a, b) => b[1].count - a[1].count);

const us = frames.reduce((a, r) => a + r.us, 0);
const lines = [];
lines.push(`window ${hex(FROM)} -> ${hex(TO)}   ${frames.length} game frames, ` +
           `${total.toLocaleString()} i960 instructions, ${(us / 1000).toFixed(1)} ms of host time`);
if (frames.length) {
    const worst = frames.reduce((a, b) => (b.us > a.us ? b : a));
    lines.push(`worst frame ${worst.f}: ${(worst.us / 1000).toFixed(2)} ms, ` +
               `${worst.st.toLocaleString()} steps; mean ${(us / frames.length / 1000).toFixed(2)} ms`);
}
lines.push('');
lines.push('  share      instructions  routine');
for (const [name, e] of ranked.slice(0, TOP))
    lines.push(`  ${(100 * e.count / total).toFixed(2).padStart(6)}%  ${String(e.count).padStart(14)}  ${name} (${hex(e.start)})`);
const report = lines.join('\n');
fs.writeFileSync(path.join(OUT, 'report.txt'), report + '\n');
fs.writeFileSync(path.join(OUT, 'by-routine.csv'),
    'routine,start,instructions\n' + ranked.map(([n, e]) => `${n},${hex(e.start)},${e.count}`).join('\n') + '\n');
console.log(report);
console.log(`\n-> ${OUT}`);
