/*
 * match-replay.mjs — attract mode's preprogrammed Sonic vs Bean fight, this
 * emulator against MAME, frame by frame.
 *
 * Sonic The Fighters' first attract fight is not the CPU playing: it is a
 * replay. replay_bank_init_data (ROM 0xDC9B0) holds both players' inputs, a
 * byte each per frame, and key_play_disp feeds them to the fighters. So the
 * fight is the same on every board, and any difference between two emulators
 * playing it is a difference in how they simulate it — a collision reply, a
 * motion blend, a float rounded the other way — not in what anyone pressed.
 *
 * Getting there normally means sitting through ~2200 frames of intro movie,
 * which costs MAME about half an hour under -nodrc. match_replay skips it:
 * at the movie step (_sub_mode 5) it writes the movie state a natural boot has
 * when the replay starts and moves on to the replay (see attract_replay in
 * src/profiles/sfight.h). The fight that follows is bit for bit the natural
 * boot's, on stage 1 with its poles and barriers. m2hle does it with
 * --match-replay; MAME with tools/mame/match-replay.lua. Both sample both
 * fighters' whole work structures at variable_diff_calc.
 *
 *   node tools/match-replay.mjs --mame              # take MAME's reference (~12 min for 1400 frames)
 *   node tools/match-replay.mjs                     # play it here and grade against it
 *   node tools/match-replay.mjs --ref <file> --frames 1300 --show 8
 *
 * $MAME_EXE names mame.exe (default ../claude_mame/mame/mame.exe) and
 * $MAME_ROMPATH a directory holding only sfight.zip, schamp.zip and
 * segabill.zip (default tools/mame/mameroms), as tools/mame/cop_capture.py.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { REPO } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['ref', 'frames', 'port', 'show', 'out']);
const rep = new Report('match-replay — the attract replay fight, emulator vs MAME');

const FRAMES = args.num('frames', 1300);
const OUT = args.str('out', path.join(os.tmpdir(), 'm2hle-match-replay'));
const REF = args.str('ref', path.join(OUT, 'mame.bin'));
const SHOW = args.num('show', 6);
const ROBS = [0x510d00, 0x514100];
/* A fighter's whole work structure: fa_rob1 sits 0x3400 after fa_rob0. */
const ROB = 0x3400;
/* Bufferram the coprocessor writes and the i960 reads straight back, not
 * through the FIFO: both fighters' TGP slots (op 0x67 and the IK store them;
 * op 0x69 loads them) and the unit-overlap table op 0x3B leaves for
 * coli_attack_chk. A reply can be exact and these still differ. */
const EXTRA = [[0x90e800, 0x800], [0x90f600, 0x100]];
const EXTRA_BYTES = EXTRA.reduce((n, [, l]) => n + l, 0);
const REC = 8 + 2 * ROB + EXTRA_BYTES;

/* Which words of a fighter's work structure the fight is: its state, position,
 * angles, velocities, motion, energy and requests. 0x140..0x157 is the eye
 * blink and gaze, which draw from rand() and so from the board timers, and
 * nothing the fighters do reads them back. 0x1F8 on is the rig — joint
 * positions the coprocessor hands back for drawing and the eyes. */
const BLINK = [0x140, 0x158];
const RIG = 0x1f8;

/* ---- MAME's reference ------------------------------------------------------ */

if (args.bool('mame')) {
    const exe = process.env.MAME_EXE ?? path.resolve(REPO, '..', 'claude_mame', 'mame', 'mame.exe');
    const rompath = process.env.MAME_ROMPATH ?? path.join(REPO, 'tools', 'mame', 'mameroms');
    if (!fs.existsSync(exe)) { console.error(`no MAME at ${exe} — set $MAME_EXE`); process.exit(2); }
    for (const z of ['sfight.zip', 'schamp.zip', 'segabill.zip']) {
        if (!fs.existsSync(path.join(rompath, z))) { console.error(`${rompath} has no ${z} — set $MAME_ROMPATH`); process.exit(2); }
    }
    /* A fresh NVRAM and cfg every time: a power-on board, and no remembered
     * DIP settings or warning acknowledgements from anyone's MAME session. */
    const stamp = String(Date.now());
    const nvram = path.join(OUT, 'nvram-' + stamp), cfg = path.join(OUT, 'cfg-' + stamp);
    fs.mkdirSync(nvram, { recursive: true });
    fs.mkdirSync(cfg, { recursive: true });
    rep.note(`running MAME headless for ${FRAMES} replay frames -> ${REF}`);
    const t0 = Date.now();
    const r = spawnSync(exe, ['sfight', '-rompath', rompath, '-nvram_directory', nvram, '-cfg_directory', cfg,
        '-nodrc', '-video', 'none', '-sound', 'none', '-nothrottle', '-skip_gameinfo', '-seconds_to_run', '299',
        '-autoboot_script', path.join(REPO, 'tools', 'mame', 'match-replay.lua')],
        { cwd: path.dirname(exe), env: { ...process.env, MR_OUT: REF, MR_FRAMES: String(FRAMES),
                                          MR_ROB: ROB.toString(16), MR_EXTRA: EXTRA.map(([a, l]) => `${a.toString(16)}:${l.toString(16)}`).join(',') },
          stdio: 'ignore' });
    const log = fs.existsSync(REF + '.log') ? fs.readFileSync(REF + '.log', 'utf8') : '';
    const got = fs.existsSync(REF) ? fs.statSync(REF).size / REC : 0;
    rep.check('MAME played the replay', /done/.test(log) && got >= FRAMES,
              `${got} frames in ${((Date.now() - t0) / 1000).toFixed(0)}s (exit ${r.status})`);
    rep.finish();
    process.exit();
}

/* ---- this emulator ------------------------------------------------------- */

if (!fs.existsSync(REF)) {
    rep.skip('the MAME reference is present', `no ${REF} — take one with --mame`);
    rep.finish();
    process.exit();
}

function loadMame(file) {
    const b = fs.readFileSync(file), frames = [];
    for (let o = 0; o + REC <= b.length; o += REC) {
        frames.push({ fc: b.readUInt32LE(o), stage: b[o + 4], robs: b.subarray(o + 8, o + 8 + 2 * ROB), extra: b.subarray(o + 8 + 2 * ROB, o + REC) });
    }
    return frames;
}

async function playHere() {
    fs.mkdirSync(OUT, { recursive: true });
    process.env.M2HLE_UNTHROTTLE = '1';
    const emu = await M2Hle.launch({ rom: findRom().primary, port: args.num('port', 7172), extraArgs: ['--match-replay'] });
    const prefix = path.join(OUT, 'here');
    try {
        await emu.waitUntilRunning();
        /* From boot: the jump is a couple of hundred frames in. */
        const r = await emu.rpc('capture_dl', {
            frames: Math.min(3600, FRAMES + 400), path: prefix,
            probes: '500020:4,500064:1,500030:1',
            blocks: [...ROBS.map((a) => [a, ROB]), ...EXTRA].map(([a, l]) => `${a.toString(16)}:${l.toString(16)}`).join(','),
            lo: 0x8cfff0, hi: 0x8d0000, max_words: 1024, timeout_ms: 900000,
        });
        const st = await emu.status();
        rep.check('the emulator took the jump', st.match_replay === 'done', `match_replay ${st.match_replay} at frame ${st.match_replay_frame}`);
        if (!r.complete) throw new Error(`capture_dl stopped after ${r.frames} frames`);
    } finally {
        await emu.close();
    }
    const meta = JSON.parse(fs.readFileSync(prefix + '.json', 'utf8'));
    const blocks = fs.readFileSync(prefix + '.blocks.bin');
    const per = 2 * ROB + EXTRA_BYTES;
    return meta.marks.map((m, i) => ({ fc: m[2] >>> 0, stage: m[3], robs: blocks.subarray(i * per, i * per + 2 * ROB),
                                       extra: blocks.subarray(i * per + 2 * ROB, (i + 1) * per) }));
}

const mame = loadMame(REF);
const here = await playHere();

/* Both start counting at the frame the replay's stage is loaded. */
const start = (frames) => frames.findIndex((f) => f.stage === 1);
const m0 = start(mame), h0 = start(here);
rep.check('both loaded the replay stage', m0 >= 0 && h0 >= 0, `MAME at frame_counter ${mame[m0]?.fc}, here at ${here[h0]?.fc}`);
const n = Math.min(mame.length - m0, here.length - h0);
rep.note(`${n} frames from the stage load compared`);

const f32 = (buf, o) => buf.readFloatLE(o);
const u16 = (buf, o) => buf.readUInt16LE(o);
function diffs(a, b, lo, hi) {
    const out = [];
    for (let p = 0; p < 2; p++) {
        for (let w = lo; w < hi; w += 4) {
            if (w >= BLINK[0] && w < BLINK[1]) continue;
            const x = a.readUInt32LE(p * ROB + w), y = b.readUInt32LE(p * ROB + w);
            if (x !== y) out.push(`P${p + 1}+${w.toString(16)} ${x.toString(16)}/${y.toString(16)} (${f32(a, p * ROB + w).toPrecision(7)}/${f32(b, p * ROB + w).toPrecision(7)})`);
        }
    }
    return out;
}

/* The fight as it reads on screen: motion changes and energy, frame by frame. */
const event = (robs) => [0, 1].map((p) => `${u16(robs, p * ROB + 0x1a8)}/${u16(robs, p * ROB + 0x1ac)}`).join(' ');
const RANGES = [
    ['state, position, angles, motion, energy (+0 .. +0x1F8)', 0, RIG],
    ['the rig: joints for drawing and the eyes (+0x1F8 .. +0x400)', RIG, 0x400],
    ['the rest of the work structure (+0x400 on)', 0x400, ROB],
];
const firstIn = RANGES.map(() => -1);
let firstEvent = -1;
for (let i = 0; i < n; i++) {
    const a = mame[m0 + i].robs, b = here[h0 + i].robs;
    RANGES.forEach(([, lo, hi], r) => { if (firstIn[r] < 0 && diffs(a, b, lo, hi).length) firstIn[r] = i; });
    if (firstEvent < 0 && event(a) !== event(b)) firstEvent = i;
}

rep.check('the fight is the same fight (every motion and energy change on the same frame)', firstEvent < 0,
          firstEvent < 0 ? `${n} frames` : `first at +${firstEvent}: MAME motion/energy ${event(mame[m0 + firstEvent].robs)}, here ${event(here[h0 + firstEvent].robs)}`);
RANGES.forEach(([name, lo, hi], r) => {
    const first = firstIn[r];
    rep.check(`both fighters bit-identical: ${name}`, first < 0, first < 0 ? `${n} frames` : `first at +${first}`);
    for (let i = first; first >= 0 && i < Math.min(n, first + SHOW); i++) {
        const d = diffs(mame[m0 + i].robs, here[h0 + i].robs, lo, hi);
        if (d.length) rep.note(`+${i}: ${d.slice(0, 6).join(', ')}${d.length > 6 ? ` … ${d.length} words` : ''}`);
    }
});
/* The coprocessor's bufferram, range by range. */
{
    let off = 0;
    for (const [addr, len] of EXTRA) {
        let first = -1, at = -1;
        for (let i = 0; i < n && first < 0; i++) {
            const a = mame[m0 + i].extra, b = here[h0 + i].extra;
            for (let k = 0; k < len; k += 4) {
                if (a.readUInt32LE(off + k) !== b.readUInt32LE(off + k)) { first = i; at = addr + k; break; }
            }
        }
        rep.note(first < 0 ? `bufferram 0x${addr.toString(16)}+0x${len.toString(16)} matches`
                           : `bufferram 0x${addr.toString(16)}+0x${len.toString(16)} first differs at +${first}, 0x${at.toString(16)}: ` +
                             `MAME ${mame[m0 + first].extra.readUInt32LE(off + at - addr).toString(16)}, here ${here[h0 + first].extra.readUInt32LE(off + at - addr).toString(16)}`);
        off += len;
    }
}
rep.finish();
