/*
 * grade-osage.mjs — the sway chains (osage: Fang's tail, Bean's feathers, Bark's
 * scarf) at character select, this emulator's coprocessor against MAME's.
 *
 * Fn_osage (op 0x4A) takes a stream of typed records the i960 lays in bufferram
 * — the matrix the chain hangs from, its limits, its start point, then one
 * record a segment — and answers a draw matrix per segment. So a chain can go
 * wrong two ways, and this tells them apart:
 *
 *   the port    replay MAME's capture of the firmware's side of the FIFOs
 *               through the HLE (tests/cop_replay): every answered word exact,
 *               from the board's own records;
 *   its inputs  the ops that build the record's matrix (0x04 Fn_load_matrix
 *               loads the camera, 0x45 Fn_mul_matrix_inner composes it) must
 *               leave the board's matrix, and the record matrix this emulator
 *               hands Fn_osage must be as orthonormal as the board's.
 *
 * Character select, because attract never reaches it: osage_dsp loads the
 * camera there with 0x04, which the attract captures never send.
 *
 *   node tools/grade-osage.mjs --mame      # MAME's captures, headless (~15 min)
 *   node tools/grade-osage.mjs             # capture here and grade (~1 min)
 *   node tools/grade-osage.mjs --chars 4,10 --frames 120 --out <dir>
 *
 * Fighter ids: 4 Fang, 5 Bark, 7 Espio, 10 Bean. $MAME_EXE and $MAME_ROMPATH as
 * tools/match-replay.mjs; $COP_REPLAY names cop_replay (default the build's).
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
import { IN, P1_ROB, ROB_CHAR } from './lib/board.mjs';

const args = parseArgs(['chars', 'frames', 'out', 'port']);
const CHARS = args.str('chars', '4,10').split(',').map(Number);
const FRAMES = args.num('frames', 120);
const OUT = args.str('out', path.join(os.tmpdir(), 'm2hle-osage'));
const rep = new Report('grade-osage — sway chains at character select, emulator vs MAME');
fs.mkdirSync(OUT, { recursive: true });

/* ---- MAME's reference ------------------------------------------------------ */

if (args.bool('mame')) {
    const exe = process.env.MAME_EXE ?? path.resolve(REPO, '..', 'claude_mame', 'mame', 'mame.exe');
    const rompath = process.env.MAME_ROMPATH ?? path.join(REPO, 'tools', 'mame', 'mameroms');
    if (!fs.existsSync(exe)) { console.error(`no MAME at ${exe} — set $MAME_EXE`); process.exit(2); }
    for (const z of ['sfight.zip', 'schamp.zip', 'segabill.zip']) {
        if (!fs.existsSync(path.join(rompath, z))) { console.error(`${rompath} has no ${z} — set $MAME_ROMPATH`); process.exit(2); }
    }
    const stamp = String(Date.now());
    const dirs = ['nvram', 'cfg', 'snap'].map((d) => path.join(OUT, `${d}-${stamp}`));
    dirs.forEach((d) => fs.mkdirSync(d, { recursive: true }));
    rep.note(`running MAME headless for fighters ${CHARS.join(',')} -> ${OUT}/mame-c*`);
    const t0 = Date.now();
    const r = spawnSync(exe, ['sfight', '-rompath', rompath, '-nvram_directory', dirs[0], '-cfg_directory', dirs[1],
        '-snapshot_directory', dirs[2], '-nodrc', '-video', 'none', '-sound', 'none', '-nothrottle', '-skip_gameinfo',
        '-seconds_to_run', '290', '-autoboot_script', path.join(REPO, 'tools', 'mame', 'osage-select.lua')],
        { cwd: path.dirname(exe), stdio: 'ignore',
          env: { ...process.env, OS_OUT: path.join(OUT, 'mame'), OS_CHARS: CHARS.join(','), OS_FRAMES: String(FRAMES) } });
    const log = fs.existsSync(path.join(OUT, 'mame.log')) ? fs.readFileSync(path.join(OUT, 'mame.log'), 'utf8') : '';
    for (const ch of CHARS) {
        rep.check(`MAME captured fighter ${ch} at select`, new RegExp(`char ${ch}: ok`).test(log),
                  `${((Date.now() - t0) / 1000).toFixed(0)}s, exit ${r.status}; snapshots in ${dirs[2]}`);
    }
    rep.finish();
    process.exit();
}

/* ---- this emulator --------------------------------------------------------- */

const replayExe = process.env.COP_REPLAY ?? ['build/Release/cop_replay.exe', 'build/cop_replay']
    .map((p) => path.join(REPO, p)).find((p) => fs.existsSync(p));
if (!replayExe) { rep.skip('cop_replay is built', 'build the cop_replay target, or set $COP_REPLAY'); rep.finish(); process.exit(); }
const missing = CHARS.filter((ch) => !fs.existsSync(path.join(OUT, `mame-c${ch}.bin`)));
if (missing.length) {
    rep.skip('the MAME captures are present', `none for fighter ${missing.join(',')} in ${OUT} — take them with --mame`);
    rep.finish();
    process.exit();
}

/* To character select the way a player gets there, the cursor onto the fighter
 * and a capture of the coprocessor conversation (capture_dl cop:1, a MAME
 * SHARC-side capture's format). A fresh boot a fighter: after one capture the
 * cursor no longer answers the stick, so a second fighter in the same run
 * would silently be the first one again. */
async function captureHere(ch) {
    process.env.M2HLE_UNTHROTTLE = '1';
    const emu = await M2Hle.launch({ rom: findRom().primary, port: args.num('port', 7172) });
    const tap = async (bits, frames = 6) => { await emu.setInput(bits); await emu.waitFrames(frames); await emu.setInput(0); await emu.waitFrames(frames); };
    const charNow = async () => (await emu.readMemory(P1_ROB + ROB_CHAR, 1))[0];
    try {
        await emu.waitUntilRunning();
        await emu.waitFrames(900, 120000);
        await tap(0x1);                                   /* COIN1 */
        await emu.waitFrames(60);
        await tap(IN.START1);
        await emu.waitFrames(400, 120000);
        for (let i = 0; i < 12 && await charNow() !== ch; i++) await tap(IN.P1_RIGHT, 12);
        await emu.waitFrames(40);
        const got = await charNow();
        const r = await emu.rpc('capture_dl', { cop: 1, frames: FRAMES, path: path.join(OUT, `here-c${ch}`),
                                               max_words: 16 * 1024 * 1024, timeout_ms: 600000 });
        return rep.check(`here: fighter ${ch} captured at select`, got === ch && r.complete, `cursor on ${got}, ${r.words} words`);
    } finally {
        await emu.close();
    }
}

/* One cop_replay run: the op table, the state table and the osage dump. */
function replay(prefix) {
    const dump = prefix + '.osage.txt';
    const r = spawnSync(replayExe, [prefix, '0'], { encoding: 'utf8', maxBuffer: 64 << 20, env: { ...process.env, OSAGE: dump } });
    const ops = {}, states = {};
    for (const line of r.stdout.split('\n')) {
        let m = line.match(/^[ *]([0-9A-F]{2})  (\S+)\s+(\d+)\s+\S+\s+\d+!?\s+\S+\s+\S+\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)/);
        if (m) { ops[m[1]] = { name: m[2], count: +m[3], words: +m[4], exact: +m[5], close: +m[6], wrong: +m[7], missing: +m[8], extra: +m[9] }; continue; }
        m = line.match(/^[ *]([0-9A-F]{2})\s{13}(\S+)\s+(\d+)\s+(\d+)/);
        if (m) states[m[1]] = { name: m[2], n: +m[3], bad: +m[4] };
    }
    return { ops, states, shape: recordShape(dump) };
}

/* How far each record's hanging matrix (type 1's first 12 words) is from
 * orthonormal, and how many segments were answered. A matrix with a zero
 * column is a chain scaled away (the select screen closing) and is counted,
 * not measured. */
function recordShape(file) {
    let calls = 0, segs = 0, norm = 0, cos = 0, hidden = 0;
    if (!fs.existsSync(file)) return null;
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
        const t = line.trim().split(/\s+/);
        if (line.startsWith('call')) calls++;
        else if (t[0] === '->') segs++;
        else if (t[0] === '1:') {
            const v = t.slice(1, 10).map(Number), cols = [v.slice(0, 3), v.slice(3, 6), v.slice(6, 9)];
            const len = cols.map((c) => Math.hypot(...c));
            if (len.some((l) => l === 0)) { hidden++; continue; }
            len.forEach((l) => { norm = Math.max(norm, Math.abs(l - 1)); });
            for (const [a, b] of [[0, 1], [0, 2], [1, 2]]) {
                cos = Math.max(cos, Math.abs(cols[a].reduce((s, x, i) => s + x * cols[b][i], 0)) / (len[a] * len[b]));
            }
        }
    }
    return { calls, segs, norm, cos, hidden };
}

const TOL = 1e-3;
for (const ch of CHARS) {
    rep.note(`fighter ${ch}`);
    if (!await captureHere(ch)) continue;
    const mame = replay(path.join(OUT, `mame-c${ch}`));
    const here = replay(path.join(OUT, `here-c${ch}`));
    const o = mame.ops['4A'];
    rep.check('MAME: every Fn_osage word the port answers is the board\'s', o && o.words > 0 && o.exact === o.words,
              o ? `${o.exact}/${o.words} exact over ${o.count} calls` : 'no Fn_osage in the capture');
    for (const op of ['04', '45']) {
        const s = mame.states[op];
        if (!s) { rep.skip(`MAME: op ${op} leaves the board's matrix`, 'not sent in this capture'); continue; }
        rep.check(`MAME: ${s.name} (${op}) leaves the board's matrix`, s.bad === 0, `${s.bad} of ${s.n} checks off`);
    }
    for (const [who, r] of [['MAME', mame], ['here', here]]) {
        const sh = r.shape;
        rep.check(`${who}: the matrix each chain hangs from is orthonormal`, sh && sh.calls > 0 && sh.norm < TOL && sh.cos < TOL,
                  sh ? `max |len-1| ${sh.norm.toFixed(5)}, max |cos| ${sh.cos.toFixed(5)} over ${sh.calls} calls, ${sh.segs} segments${sh.hidden ? `, ${sh.hidden} scaled to zero` : ''}` : 'no dump');
    }
    const h = here.ops['4A'];
    rep.check('here: the capture replays (Fn_osage exact against itself)', h && h.words > 0 && h.exact === h.words,
              h ? `${h.exact}/${h.words}` : 'no Fn_osage in the capture');
}
rep.finish();
