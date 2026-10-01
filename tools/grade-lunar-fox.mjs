/*
 * grade-lunar-fox.mjs — the Death Egg II cutscene (MEZASE_DEATHEGG: the Lunar
 * Fox rolls out of Tails' lab and takes off), this emulator's pictures against
 * MAME's.
 *
 * Nothing in attract plays it: a 1P game has to win its ninth fight, on Giant
 * Wing. Both emulators get there by the same cheat (tools/mame/lunar-fox.lua
 * on MAME, playHere() below): coin, Start, the fighter in --char picked at
 * select (the one who flies the Lunar Fox), STAGE_ID held at 8 through select
 * so ROUND_INIT loads stage 7, and every round won by P1 on time (P1's energy
 * held full, P2's at 1, the round clock cut to a second). VIC_DSP then moves to
 * sub-mode 0x1E itself, as on the board.
 *
 * The cutscene runs off am_cntr (0x5004C4, stepped once a frame by
 * MEZASE_DEATHEGG_DSP), which starts again at 0 when the scene changes from
 * the lab to space. A picture is paired with MAME's by (part, am_cntr), so the
 * cheat's fight may take a different number of frames on each board.
 *
 * The board's own figure of merit is the emblem on the lab doors (the Tails
 * head, --crop x,y,w,h): per pair, the mean difference a channel over the
 * crop and over the whole frame, and a MAME | here strip of the worst crops.
 * The check: while the doors are in shot and clear of smoke (am_cntr < 195 of
 * the lab part), the
 * crop's worst pair within --tol (3) a channel. --set zflat=0 passes a
 * set_camera key here, for an A/B.
 *
 *   node tools/grade-lunar-fox.mjs --mame [--char 0]   # MAME's snapshots (~10 min)
 *   node tools/grade-lunar-fox.mjs [--char 0]          # play it here and grade
 *
 * $MAME_EXE (default: the Linux build in ~/build/mame-bin, else
 * ../claude_mame/mame/mame.exe) and $MAME_ROMPATH as tools/match-replay.mjs.
 */
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom, mameRomPath } from './lib/rom.mjs';
import { REPO } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { readPng, writePng } from './lib/png.mjs';
import { COIN1, IN } from './lib/board.mjs';

const args = parseArgs(['char', 'every', 'port', 'out', 'lag', 'crop', 'set', 'part', 'tol']);
/* --fixrand: rand() made a plain counter and `random` pinned into the cutscene
 * on both boards (see lunar-fox.lua), so the launch smoke is laid out alike.
 * Without it the smoke follows live timer counts and cannot match. Both sides
 * must be taken with it. */
const FIXRAND = args.bool('fixrand');
const RANDOM = 0x500098, RANDOM_SEED = 0x13572468;
const CHAR = args.num('char', 0);
const EVERY = args.num('every', 10);
const OUT = path.resolve(args.str('out', path.join(os.tmpdir(), 'm2hle-lunar-fox', `c${CHAR}`)));
const W = 496, H = 384;
const [CX, CY, CW, CH] = args.str('crop', '196,140,104,70').split(',').map(Number);
/* set_camera switches to play with here, for an A/B: --set zflat=0,checker=0 */
const SET = Object.fromEntries((args.str('set', '') || '').split(',').filter(Boolean).map((kv) => kv.split('=')));
const rep = new Report(`grade-lunar-fox — the Death Egg II cutscene, emulator vs MAME (fighter ${CHAR})`);

const MODE = 0x50002a, SUB = 0x500030, STAGE_ID = 0x500054, GAME_TIMER = 0x500028, AM_CNTR = 0x5004c4;
const FA_ROB0 = 0x500804, FA_ROB1 = 0x500808, ROB_ENERGY = 0x1ac, ROB_CHAR = 0x1b0, P1_ROB = 0x510d00;
const SUB_MASK_INT = 30, SUB_DSP = 33;

/* ---- MAME's side ---------------------------------------------------------- */

const mameDir = path.join(OUT, 'mame');
if (args.bool('mame')) {
    const linux = path.join(os.homedir(), 'build', 'mame-bin', 'claude_mame-mame', 'm2');
    const exe = process.env.MAME_EXE ?? (fs.existsSync(linux) ? linux : path.resolve(REPO, '..', 'claude_mame', 'mame', 'mame.exe'));
    if (!fs.existsSync(exe)) { console.error(`no MAME at ${exe} — set $MAME_EXE`); process.exit(2); }
    const dirs = ['nvram', 'cfg', 'snap'].map((d) => path.join(mameDir, d));
    for (const d of dirs) fs.mkdirSync(d, { recursive: true });
    const snaps = path.join(dirs[2], 'sfight');
    if (fs.existsSync(snaps)) for (const f of fs.readdirSync(snaps)) fs.rmSync(path.join(snaps, f));
    rep.note(`running MAME headless, fighter ${CHAR} -> ${mameDir}`);
    const r = spawnSync(exe, ['sfight', '-rompath', mameRomPath(), '-nvram_directory', dirs[0], '-cfg_directory', dirs[1],
        '-snapshot_directory', dirs[2], '-nodrc', '-video', 'none', '-sound', 'none', '-nothrottle', '-skip_gameinfo',
        '-seconds_to_run', '299', '-autoboot_script', path.join(REPO, 'tools', 'mame', 'lunar-fox.lua')],
        { stdio: 'ignore', timeout: 3600000,
          env: { ...process.env, SDL_VIDEODRIVER: process.env.SDL_VIDEODRIVER ?? 'dummy',
                 LF_OUT: path.join(mameDir, 'mame'), LF_CHAR: String(CHAR), LF_EVERY: String(EVERY),
                 LF_FIXRAND: FIXRAND ? '1' : '0' } });
    const log = fs.existsSync(path.join(mameDir, 'mame.log')) ? fs.readFileSync(path.join(mameDir, 'mame.log'), 'utf8') : '';
    rep.check('MAME played the cutscene through', /^done$/m.test(log), r.error ? String(r.error) : `exit ${r.status}`);
    rep.finish();
    process.exit(0);
}

/* MAME's snapshots in order, each with its (part, am_cntr). */
const mameLog = path.join(mameDir, 'mame.log');
if (!fs.existsSync(mameLog)) {
    rep.skip('MAME snapshots are present', `none in ${mameDir} — take them with --mame`);
    rep.finish();
    process.exit(0);
}
const mame = [];
{
    let part = 0, last = -1;
    const files = fs.readdirSync(path.join(mameDir, 'snap', 'sfight')).filter((f) => f.endsWith('.png')).sort();
    for (const m of fs.readFileSync(mameLog, 'utf8').matchAll(/snapshot am (\d+) frame (\d+)/g)) {
        const am = Number(m[1]);
        if (am < last) part++;
        last = am;
        mame.push({ part, am, file: path.join(mameDir, 'snap', 'sfight', files[mame.length]) });
    }
}

/* ---- here ----------------------------------------------------------------- */

async function playHere(port) {
    const avPort = port + 100;
    const emu = await M2Hle.launch({ rom: findRom().primary, port,
        extraArgs: ['--av-port', String(avPort), '--av-size', `${W}x${H}`] });
    const pictures = new Map();
    let sock = null, keep = false;
    const u8 = async (a) => (await emu.readMemory(a, 1))[0];
    const u32 = async (a) => (await emu.readMemory(a, 4)).readUInt32LE(0);
    const w16 = (a, v) => emu.writeMemory(a, [v & 0xff, (v >> 8) & 0xff]);
    const tap = async (bits, frames = 8) => { await emu.setInput(bits); await emu.waitFrames(frames); await emu.setInput(0); await emu.waitFrames(frames); };
    const until = async (what, cond, ms = 300000) => {
        for (const end = Date.now() + ms; !(await cond());) {
            if (Date.now() > end) throw new Error(`never reached ${what}`);
            await emu.waitFrames(1);
        }
    };
    try {
        if (Object.keys(SET).length) await emu.rpc('set_camera', SET);
        sock = net.connect(avPort, '127.0.0.1');
        let buf = Buffer.alloc(0), hdr = false;
        sock.on('error', () => {});
        sock.on('data', (d) => {
            buf = Buffer.concat([buf, d]);
            for (;;) {
                if (!hdr) { if (buf.length < 32) return; buf = buf.subarray(32); hdr = true; }
                if (buf.length < 24) return;
                const size = buf.readUInt32LE(4);
                if (buf.length < 24 + size) return;
                if (buf[0] === 0x56 && keep) pictures.set(Number(buf.readBigUInt64LE(8)), Buffer.from(buf.subarray(24, 24 + size)));
                buf = buf.subarray(24 + size);
            }
        });
        await emu.waitForRom();
        if (FIXRAND) {
            const w = (a, v) => emu.rpc('write_memory', { addr: '0x' + a.toString(16), data: v.toString(16).padStart(8, '0').match(/../g).reverse().join(''), rom: '1' });
            await w(0x66bc, 0x5c681e01);
            for (const a of [0x66c8, 0x66d4, 0x66e0]) await w(a, 0x5c681e00);
        }
        await emu.waitFrames(900);
        await tap(COIN1);
        await emu.waitFrames(60);
        await tap(IN.START1);
        await until('character select', async () => await u8(MODE) === 7 && await u8(SUB) === 5);
        await emu.waitFrames(400);
        for (let i = 0; i < 12 && await u8(P1_ROB + ROB_CHAR) !== CHAR; i++) await tap(IN.P1_RIGHT, 10);
        const picked = await u8(P1_ROB + ROB_CHAR);
        rep.check(`fighter ${CHAR} picked here`, picked === CHAR, `cursor on ${picked}`);
        await tap(IN.P1_B2);
        /* the rounds, cheated as lunar-fox.lua does, until MEZASE_DEATHEGG_MASK_INT */
        let full = null;
        for (const end = Date.now() + 600000; ;) {
            const mode = await u8(MODE), sub = await u8(SUB);
            if (sub === SUB_MASK_INT) break;
            if (mode === 3) throw new Error('the game went back to attract: P1 lost');
            if (mode === 7 && sub <= 7) await emu.writeMemory(STAGE_ID, [8]);
            if (sub === 8) full = (await emu.readMemory(await u32(FA_ROB0) + ROB_ENERGY, 2)).readUInt16LE(0);
            if (sub === 9 && full !== null) {
                await w16(await u32(FA_ROB0) + ROB_ENERGY, full);
                await w16(await u32(FA_ROB1) + ROB_ENERGY, 1);
                if ((await emu.readMemory(GAME_TIMER, 2)).readInt16LE(0) > 60) await w16(GAME_TIMER, 60);
            }
            if (Date.now() > end) throw new Error(`the cutscene never came (mode ${mode}, sub ${sub})`);
            await emu.waitFrames(1);
        }
        rep.note(`MEZASE_DEATHEGG_MASK_INT reached here, stage ${await u8(0x500064)}`);
        keep = true;
        if (FIXRAND) {
            /* a frame at a time, as lunar-fox.lua's frame hook does */
            await emu.rpc('emu_stop', {});
            for (let sub = await u8(SUB); sub >= SUB_MASK_INT && sub < SUB_DSP; sub = await u8(SUB)) {
                await emu.writeMemory(RANDOM, [RANDOM_SEED & 0xff, (RANDOM_SEED >> 8) & 0xff, (RANDOM_SEED >> 16) & 0xff, RANDOM_SEED >>> 24]);
                /* the stop lands at a frame edge, a moment after emu_stop answers */
                for (let k = 0; !(await emu.rpc('run_frames', { count: 1 }, { allowFail: true })).ok; k++) {
                    if (k > 200) throw new Error('the board never stopped');
                    await new Promise((r2) => setTimeout(r2, 5));
                }
            }
            await emu.rpc('emu_run', {});
        }
        const prefix = path.join(OUT, 'here');
        const r = await emu.rpc('capture_dl', {
            frames: 1100, path: prefix, probes: `${SUB.toString(16)}:4,${AM_CNTR.toString(16)}:4,${RANDOM.toString(16)}:4`,
            lo: 0x8cfff0, hi: 0x8d0000, max_words: 1024, timeout_ms: 900000,
        });
        await new Promise((r2) => setTimeout(r2, 500));   /* the last pictures in flight */
        keep = false;
        const meta = JSON.parse(fs.readFileSync(prefix + '.json', 'utf8'));
        for (const ext of ['.bin', '.json']) fs.rmSync(prefix + ext, { force: true });
        if (!r.complete) rep.note(`capture_dl stopped after ${r.frames} frames`);
        /* (part, am_cntr) -> board frame, over the frames MEZASE_DEATHEGG_DSP ran */
        const at = new Map();
        let part = 0, last = -1, done = false;
        for (const m of meta.marks) {
            const sub = m[2] & 0xff, am = m[3] & 0xffff;
            if (sub !== SUB_DSP) { if (last >= 0) done = true; continue; }
            if (done) break;
            if (am < last) part++;
            if (am !== last) at.set(`${part}:${am}`, m[0]);
            if (FIXRAND && am !== last && am % 50 === 0) rep.note(`  random at part ${part} am ${am}: ${(m[4] >>> 0).toString(16).toUpperCase()}`);
            last = am;
        }
        const keys = [...pictures.keys()], frames = [...at.values()];
        rep.note(`here: ${pictures.size} pictures (board frames ${Math.min(...keys)}..${Math.max(...keys)}), ` +
                 `cutscene on board frames ${Math.min(...frames)}..${Math.max(...frames)}`);
        return { pictures, at };
    } finally {
        if (sock) sock.destroy();
        await emu.close();
    }
}

fs.mkdirSync(OUT, { recursive: true });
for (const f of fs.readdirSync(OUT)) if (/^compare-.*\.png$/.test(f)) fs.rmSync(path.join(OUT, f));
const here = await playHere(args.num('port', 7530));

const bgra2rgb = (b) => { const o = Buffer.alloc(W * H * 3); for (let i = 0; i < W * H; i++) { o[3 * i] = b[4 * i + 2]; o[3 * i + 1] = b[4 * i + 1]; o[3 * i + 2] = b[4 * i]; } return o; };
function mae(a, b, [x0, y0, w, h] = [0, 0, W, H]) {
    let s = 0;
    for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) for (let k = 0; k < 3; k++) {
        const i = (y * W + x) * 3 + k; s += Math.abs(a[i] - b[i]);
    }
    return s / (w * h * 3);
}
const pairs = mame.filter((m) => here.at.has(`${m.part}:${m.am}`)).map((m) => ({ ...m, edge: here.at.get(`${m.part}:${m.am}`), M: readPng(m.file).rgb }));
rep.check('MAME snapshots pair with a frame here by am_cntr', pairs.length > 0, `${pairs.length} of ${mame.length} paired`);

let lag = args.num('lag', null);
if (lag === null) {
    let best = Infinity;
    for (let d = -4; d <= 4; d++) {
        let s = 0, k = 0;
        for (const p of pairs) { const pic = here.pictures.get(p.edge + d); if (pic) { s += mae(bgra2rgb(pic), p.M); k++; } }
        if (k >= Math.max(1, pairs.length / 2) && s / k < best) { best = s / k; lag = d; }
    }
    rep.note(`picture lag ${lag} frames from the edge (mean abs difference ${best.toFixed(2)} a channel there)`);
}

const CROP = [CX, CY, CW, CH];
const rows = [];
for (const p of pairs) {
    const pic = here.pictures.get(p.edge + lag);
    if (!pic) continue;
    const A = bgra2rgb(pic);
    rows.push({ ...p, A, whole: mae(A, p.M), crop: mae(A, p.M, CROP) });
}
for (const r of rows) rep.note(`part ${r.part} am ${String(r.am).padStart(3)}: frame ${r.whole.toFixed(2)}, emblem crop ${r.crop.toFixed(2)}`);

/* MAME | here, whole frame and the emblem crop enlarged, for the lab part. */
function strip(file, panels, [x0, y0, w, h], z) {
    const out = Buffer.alloc(w * z * panels.length * h * z * 3);
    for (let y = 0; y < h * z; y++) for (let k = 0; k < panels.length; k++) for (let x = 0; x < w * z; x++) {
        const s = ((y0 + ((y / z) | 0)) * W + x0 + ((x / z) | 0)) * 3, d = (y * w * z * panels.length + k * w * z + x) * 3;
        panels[k].copy(out, d, s, s + 3);
    }
    writePng(file, { width: w * z * panels.length, height: h * z, rgb: out });
}
const part = args.num('part', 0);
for (const r of rows.filter((x) => x.part === part)) {
    const stem = path.join(OUT, `compare-p${r.part}-am${String(r.am).padStart(3, '0')}`);
    strip(stem + '.png', [r.M, r.A], [0, 0, W, H], 1);
    strip(stem + '-crop.png', [r.M, r.A], CROP, 4);
}
rep.note(`MAME | here: ${OUT}/compare-p${part}-am*.png (and -crop.png, the emblem x4)`);
rep.check('graded frames', rows.length > 0, `${rows.length} frames`);
/* The doors stay in shot until am_cntr 200, where the camera goes overhead for
 * the launch, but from 195 the launch smoke crosses the crop, and its puffs
 * scatter by rand, which reads the board's timers (--fixrand pins it). Each door's emblem is a quad 0.03 in front of the door, which
 * the depth buffer cannot hold apart at that range (geo3d_mesh_layers, HELD):
 * before the fix half of it striped through to the door, 8.4 a channel. */
const doors = rows.filter((r) => r.part === 0 && r.am < 195);
const TOL = args.num('tol', 3);
if (doors.length) {
    const worst = doors.reduce((w, r) => (r.crop > w.crop ? r : w));
    rep.check(`the emblem while the doors are in shot (am < 195) within ${TOL} a channel of MAME`, worst.crop <= TOL,
              `mean ${(doors.reduce((s, r) => s + r.crop, 0) / doors.length).toFixed(2)}, worst ${worst.crop.toFixed(2)} at am ${worst.am}`);
}
rep.finish();
