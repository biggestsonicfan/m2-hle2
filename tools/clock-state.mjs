/*
 * clock-state.mjs — does the sound board keep time with the game's frames
 * through ONE game state?
 *
 * SLICE-CLOCKS.md's acceptance test. The board is driven to a state's entry
 * (`--from`) and run to whatever the ROM hands over to (`--to`), and the frame
 * counter and the SCSP's sample counter are read either side. The ratio is the
 * number that matters: 44100 / 60 = 735 samples a game frame everywhere, or the
 * board's audio clock is running at a different rate from its video clock.
 *
 * Unlike bench-state this is not a timing, so there is nothing to average: one
 * run per build is the measurement (the entry frame wanders by a few, which
 * moves the counts and not the ratio).
 *
 *   node tools/clock-state.mjs <exeA> [exeB ...] --state round-mask
 *   node tools/clock-state.mjs <exe> --state movie-egg --args "--live-timers"
 *
 * `--args` is passed to every build (e.g. --live-timers, --steps-per-slice N).
 */
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { parseArgs } from './lib/args.mjs';
import { driveToAddr, fightInputAt, attractInputAt } from './lib/drive.mjs';

const STATES = {
    'round-mask': { from: 0xB820, to: 0xC34C, drive: 'fight' },
    'movie-egg':  { from: 0x5320C, to: 0x541DC, drive: 'attract' },
};

const args = parseArgs(['from', 'to', 'state', 'rom', 'port', 'drive', 'max-frames', 'args']);
const exes = process.argv.slice(2).filter((a) => !a.startsWith('--') && /\.exe$|m2hle$/i.test(a))
    .map((a) => path.resolve(a));
if (!exes.length) { console.error('usage: node tools/clock-state.mjs <exeA> [exeB ...] [--state round-mask]'); process.exit(2); }

const named = STATES[args.str('state', 'round-mask')] ?? {};
const FROM  = args.str('from') ? Number(args.str('from')) : named.from;
const TO    = args.str('to')   ? Number(args.str('to'))   : named.to;
const DRIVE = args.str('drive', named.drive ?? 'fight');
const ROM   = path.resolve(args.str('rom', 'C:/Users/bigge/source/repos/ai/m2-hle2/test_m2snake/sfight.zip'));
const PORT  = args.num('port', 7361);
const MAXF  = args.num('max-frames', 12000);
const EXTRA = args.str('args', '').split(/\s+/).filter(Boolean);
if (FROM === undefined || TO === undefined) { console.error('need --from and --to (or a known --state)'); process.exit(2); }

process.env.M2HLE_UNTHROTTLE = '1';
const hex = (n) => '0x' + (n >>> 0).toString(16).toUpperCase();

function rundir(tag) {
    const dir = path.join(os.tmpdir(), 'm2hle-clock-' + tag);
    fs.mkdirSync(dir, { recursive: true });
    for (const z of ['sfight.zip', 'schamp.zip']) {
        const src = path.join(path.dirname(ROM), z);
        if (fs.existsSync(src)) fs.copyFileSync(src, path.join(dir, z));
    }
    return path.join(dir, path.basename(ROM));
}

async function clocks(emu) {
    const st = await emu.status();
    const snd = await emu.rpc('sound_status');
    return { frames: Number(st.frames), samples: Number(snd.samples), steps: Number(st.steps ?? 0),
             writes: Number(snd.midi_writes), drains: Number(snd.midi_drains),
             holds: Number(snd.midi_holds ?? NaN), drops: Number(snd.midi_drops) };
}

async function measure(exe, tag, port) {
    const emu = await M2Hle.launch({ exe, rom: rundir(tag), port, run: true, extraArgs: EXTRA });
    try {
        await emu.waitForRom();
        const at = await driveToAddr(emu, FROM, {
            inputAt: DRIVE === 'fight' ? fightInputAt : attractInputAt, maxFrames: MAXF,
        });
        await emu.clearAllBreakpoints();
        await emu.setBreakpoint(TO, 'to');
        const c0 = await clocks(emu);
        await emu.run();
        await emu.waitForStop(300000);
        const st = await emu.status();
        if (Number(st.ip) !== TO) throw new Error(`the window ended at ${st.ip}, not ${hex(TO)}`);
        const c1 = await clocks(emu);
        const d = {};
        for (const k of Object.keys(c0)) d[k] = c1[k] - c0[k];
        return { at, ...d };
    } finally { await emu.close(); }
}

console.log(`window ${hex(FROM)} -> ${hex(TO)}${EXTRA.length ? '   args: ' + EXTRA.join(' ') : ''}`);
for (let i = 0; i < exes.length; i++) {
    const tag = String.fromCharCode(97 + i);
    const m = await measure(exes[i], tag, PORT + i);
    console.log(`  ${tag}  ${exes[i]}`);
    console.log(`     ${m.frames} frames, ${m.samples.toLocaleString()} samples = ` +
                `${(m.samples / m.frames).toFixed(1)} a frame (735 is right), ` +
                `${(m.steps / 1e6).toFixed(2)}M instructions, entered at frame ~${m.at}`);
    console.log(`     MIDI: ${m.writes} bytes, ${m.drains} catch-up steps, ` +
                `${Number.isNaN(m.holds) ? '?' : m.holds} held to a later slice, ${m.drops} dropped`);
}
