/*
 * grade-round-intro.mjs — the 1P round intro's camera sweep on each stage,
 * this emulator's pictures against MAME's (Pinboard #249).
 *
 * The intro is where the camera passes closest to the arena's set pieces
 * (Casino Night's slot cabinet, #244), so it is where a depth or clipping
 * error shows first. Both boards play the same inputs from power-on (a coin,
 * Start, Punch and Kick on a fixed frame schedule: tools/mame/round-intro.lua)
 * with the stage forced: MAME rewrites STAGE_NUM as the game writes it, here
 * the stage is written at ROUND_STAGE_PIN (0xAFC8), before ROUND_INIT loads it.
 *
 * A picture is paired with MAME's when the camera (0x519E98, 16 bytes) is
 * bit-identical and MAME's frame index is this board frame + 1, so a frame
 * the camera holds still on is not paired with the wrong moment. A pair whose
 * mean difference is 20 a channel or more is a scene change landing a frame
 * apart (the fade-in, the stage card) and is counted, not graded, and so is
 * the NEXT MATCH screen before the round (sub-mode 1 in MAME's record),
 * whose portraits slide in a frame apart on the two boards. The
 * measure is the pixels more than 48 off MAME in any channel: per stage the
 * median over the pairs and the worst, which is written as a MAME | here |
 * difference strip.
 *
 * What remains when this was written (median pixels, 496x384, zflat on): ring
 * spin phase, the 10K sign, water and texture speckle, the Death Egg's floor
 * scroll, the laser rails' blink and the wipe's timing. None of it geometry.
 *
 *   node tools/grade-round-intro.mjs --mame [--stage 5]   # MAME's side, ~2 min a stage
 *   node tools/grade-round-intro.mjs [--stage 5]          # play it here and grade
 *   node tools/grade-round-intro.mjs --stage 5 --toggle zflat
 *
 * --toggle KEY plays each stage twice here, with the set_camera key on and off,
 * and counts the pixels that differ between the two by which is nearer MAME.
 * --set k=v,... passes set_camera keys to every run. --stage takes a list
 * (0,4,5) and defaults to stages 0-13 (stage 14 replays as stage 0).
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
import { COIN1, IN, ROUND_STAGE_PIN, STAGE_NUM } from './lib/board.mjs';

const args = parseArgs(['stage', 'port', 'out', 'toggle', 'set', 'frames', 'snap']);
const STAGES = args.str('stage', '0,1,2,3,4,5,6,7,8,9,10,11,12,13').split(',').map(Number);
const OUT = path.resolve(args.str('out', path.join(os.tmpdir(), 'm2hle-round-intro')));
const W = 496, H = 384, CAM = 0x519e98, REC = 22;
const FRAMES = args.num('frames', 500);              /* board frames captured here after the pin */
const SNAP = args.str('snap', '745:1240:3');         /* MAME frames snapshotted: first:last:step */
const OFF = 48, SCENE = 20, SUB_ROUND = 2;
const TOGGLE = args.str('toggle', null);
const SET = Object.fromEntries((args.str('set', '') || '').split(',').filter(Boolean).map((kv) => kv.split('=')));
const rep = new Report('grade-round-intro — the 1P round intro on each stage, emulator vs MAME');

/* ---- MAME's side ---------------------------------------------------------- */

if (args.bool('mame')) {
    const linux = path.join(os.homedir(), 'build', 'mame-bin', 'claude_mame-mame', 'm2');
    const exe = process.env.MAME_EXE ?? (fs.existsSync(linux) ? linux : path.resolve(REPO, '..', 'claude_mame', 'mame', 'mame.exe'));
    if (!fs.existsSync(exe)) { console.error(`no MAME at ${exe} — set $MAME_EXE`); process.exit(2); }
    const last = Number(SNAP.split(':')[1]);
    for (const s of STAGES) {
        const dir = path.join(OUT, `stage${s}`);
        const dirs = ['nvram', 'cfg', 'snap'].map((d) => path.join(dir, d));
        for (const d of dirs) fs.mkdirSync(d, { recursive: true });
        for (const f of fs.readdirSync(dirs[2])) if (f.endsWith('.png')) fs.rmSync(path.join(dirs[2], f));
        rep.note(`running MAME headless, stage ${s} -> ${dir}`);
        const r = spawnSync(exe, ['sfight', '-rompath', mameRomPath(), '-nvram_directory', dirs[0], '-cfg_directory', dirs[1],
            '-snapshot_directory', dirs[2], '-nodrc', '-video', 'none', '-sound', 'none', '-nothrottle', '-skip_gameinfo',
            '-seconds_to_run', '299', '-autoboot_script', path.join(REPO, 'tools', 'mame', 'round-intro.lua')],
            { stdio: 'ignore', timeout: 1800000, cwd: path.dirname(exe),
              env: { ...process.env, SDL_VIDEODRIVER: process.env.SDL_VIDEODRIVER ?? 'dummy',
                     RD_OUT: path.join(dir, 'mame.bin'), RD_STAGE: String(s), RD_FRAMES: String(last + 10), RD_SNAP: SNAP } });
        const log = path.join(dir, 'mame.bin.log');
        rep.check(`MAME played stage ${s}'s intro`, fs.existsSync(log) && /^done$/m.test(fs.readFileSync(log, 'utf8')),
                  r.error ? String(r.error) : `exit ${r.status}`);
    }
    rep.finish();
    process.exit(0);
}

/* ---- here ----------------------------------------------------------------- */

/* Plays to the stage's round intro and returns, for each board frame captured,
 * the camera words, and the RGB pictures of the frames `want` names. */
async function playHere(port, stage, want, camera) {
    const avPort = port + 100;
    const emu = await M2Hle.launch({ rom: findRom().primary, port, run: false,
        extraArgs: ['--av-port', String(avPort), '--av-size', `${W}x${H}`] });
    const pictures = new Map();
    let sock = null, keep = false;
    try {
        if (Object.keys(camera).length) await emu.rpc('set_camera', camera);
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
                const frame = Number(buf.readBigUInt64LE(8));
                if (buf[0] === 0x56 && keep && want(frame)) {
                    const b = buf.subarray(24, 24 + size), o = Buffer.alloc(W * H * 3);
                    for (let i = 0; i < W * H; i++) { o[3 * i] = b[4 * i + 2]; o[3 * i + 1] = b[4 * i + 1]; o[3 * i + 2] = b[4 * i]; }
                    pictures.set(frame, o);
                }
                buf = buf.subarray(24 + size);
            }
        });
        await emu.waitForRom();
        await emu.setBreakpoint(ROUND_STAGE_PIN, 'pin');
        await emu.run();
        /* round-intro.lua's schedule, a frame at a time, until the pin */
        let held = -1, pinned = false;
        for (let fc = 0; fc < 4000 && !pinned; fc++) {
            let bits = 0;
            if (fc > 300) {
                const m = fc % 60;
                if (fc % 600 < 8) bits |= COIN1;
                if (m < 6) bits |= IN.START1;
                if (m >= 20 && m < 26) bits |= IN.P1_B1;
                if (m >= 40 && m < 46) bits |= IN.P1_B2;
            }
            if (bits !== held) { await emu.setInput(bits); held = bits; }
            if ((await emu.waitFrames(1, 20000)).reached) continue;
            const st = await emu.status();
            if (st.running || Number(st.ip) !== ROUND_STAGE_PIN) throw new Error(`the board stopped at ${st.ip}`);
            await emu.writeMemory(STAGE_NUM, [stage]);
            await emu.clearAllBreakpoints();
            await emu.setInput(0);
            keep = true;
            await emu.run();
            pinned = true;
        }
        if (!pinned) throw new Error('ROUND_INIT never came');
        const prefix = path.join(OUT, `stage${stage}`, `here-${port}`);
        const r = await emu.rpc('capture_dl', { frames: FRAMES, path: prefix,
            probes: [0, 4, 8, 12].map((o) => (CAM + o).toString(16) + ':4').join(','),
            lo: 0x8cfff0, hi: 0x8d0000, max_words: 1024, timeout_ms: 900000 });
        await new Promise((r2) => setTimeout(r2, 800));   /* the last pictures in flight */
        keep = false;
        if (!r.complete) rep.note(`  capture_dl stopped after ${r.frames} frames`);
        const meta = JSON.parse(fs.readFileSync(prefix + '.json', 'utf8'));
        for (const ext of ['.bin', '.json']) fs.rmSync(prefix + ext, { force: true });
        const cams = new Map(meta.marks.map((m) => [m[0], m.slice(2, 6).map((x) => x >>> 0)]));
        return { cams, pictures };
    } finally {
        if (sock) sock.destroy();
        await emu.close();
    }
}

/* the pixels more than OFF off in any channel, and the mean difference a channel */
function diff(a, b) {
    let off = 0, sum = 0;
    for (let i = 0; i < W * H * 3; i += 3) {
        const d0 = Math.abs(a[i] - b[i]), d1 = Math.abs(a[i + 1] - b[i + 1]), d2 = Math.abs(a[i + 2] - b[i + 2]);
        if (Math.max(d0, d1, d2) > OFF) off++;
        sum += d0 + d1 + d2;
    }
    return { off, mean: sum / (W * H * 3) };
}
const median = (xs) => { const s = [...xs].sort((a, b) => a - b); return s.length ? s[s.length >> 1] : 0; };

let port = args.num('port', 7600);
for (const s of STAGES) {
    const dir = path.join(OUT, `stage${s}`);
    const bin = path.join(dir, 'mame.bin');
    if (!fs.existsSync(bin)) { rep.skip(`stage ${s}: MAME's records are present`, `none in ${dir} — take them with --mame`); continue; }
    rep.note(`stage ${s}`);
    const rec = fs.readFileSync(bin);
    const snap = (n) => path.join(dir, 'snap', `f${String(n).padStart(5, '0')}.png`);
    /* MAME's camera at index n, as four words */
    const mameCam = (n) => (n * REC + REC <= rec.length ? [0, 4, 8, 12].map((o) => rec.readUInt32LE(n * REC + 6 + o)) : null);
    const want = (f) => fs.existsSync(snap(f + 1));
    const runs = TOGGLE ? [{ ...SET, [TOGGLE]: '1' }, { ...SET, [TOGGLE]: '0' }] : [SET];
    const played = [];
    for (const camera of runs) { played.push(await playHere(port, s, want, camera)); port += 4; }

    const pairs = [];
    for (const [f, cam] of played[0].cams) {
        const m = mameCam(f + 1);
        if (!m || m.some((x, k) => x !== cam[k]) || !played.every((p) => p.pictures.has(f))) continue;
        if (TOGGLE && played[1].cams.get(f)?.some((x, k) => x !== cam[k])) continue;
        pairs.push({ f, n: f + 1, sub: rec[(f + 1) * REC + 5], M: readPng(snap(f + 1)).rgb });
    }
    if (!pairs.length) { rep.check(`stage ${s}: pictures pair with MAME's`, false, 'no frame with the same camera at the same index'); continue; }
    const rows = pairs.map((p) => ({ ...p, A: played[0].pictures.get(p.f), ...diff(played[0].pictures.get(p.f), p.M) }));
    const round = rows.filter((r) => r.sub >= SUB_ROUND), graded = round.filter((r) => r.mean < SCENE);
    if (!graded.length) { rep.check(`stage ${s}: pictures pair with MAME's in the round`, false, `${rows.length} pairs, none graded`); continue; }
    const worst = graded.reduce((w, r) => (r.off > w.off ? r : w));
    rep.check(`stage ${s}: pictures pair with MAME's`, true,
              `${rows.length} pairs, ${rows.length - round.length} on NEXT MATCH, ${round.length - graded.length} at a scene change; ` +
              `pixels > ${OFF} off MAME: ` +
              `median ${median(graded.map((r) => r.off))}, worst ${worst.off} at board frame ${worst.f}`);
    const red = Buffer.alloc(W * H * 3);
    for (let i = 0; i < W * H * 3; i += 3)
        if (Math.max(Math.abs(worst.A[i] - worst.M[i]), Math.abs(worst.A[i + 1] - worst.M[i + 1]), Math.abs(worst.A[i + 2] - worst.M[i + 2])) > OFF) red[i] = 255;
    const strip = Buffer.alloc(W * 3 * H * 3);
    for (let y = 0; y < H; y++) [worst.M, worst.A, red].forEach((p, k) => p.copy(strip, (y * W * 3 + k * W) * 3, y * W * 3, (y + 1) * W * 3));
    writePng(path.join(OUT, `worst-stage${s}.png`), { width: W * 3, height: H, rgb: strip });

    if (TOGGLE) {
        /* every pixel the key changes, by which side is nearer MAME */
        let on = 0, off = 0, changed = 0;
        for (const r of graded) {
            const B = played[1].pictures.get(r.f);
            for (let i = 0; i < W * H * 3; i += 3) {
                if (r.A[i] === B[i] && r.A[i + 1] === B[i + 1] && r.A[i + 2] === B[i + 2]) continue;
                changed++;
                const dA = Math.abs(r.A[i] - r.M[i]) + Math.abs(r.A[i + 1] - r.M[i + 1]) + Math.abs(r.A[i + 2] - r.M[i + 2]);
                const dB = Math.abs(B[i] - r.M[i]) + Math.abs(B[i + 1] - r.M[i + 1]) + Math.abs(B[i + 2] - r.M[i + 2]);
                if (dA < dB) on++; else if (dB < dA) off++;
            }
        }
        rep.check(`stage ${s}: ${TOGGLE} on is no further from MAME than off`, on >= off,
                  `${changed} pixels changed over ${graded.length} pairs: ${on} nearer MAME with ${TOGGLE} on, ${off} with it off`);
    }
}
rep.note(`MAME | here | pixels > ${OFF} off: ${OUT}/worst-stage*.png`);
rep.finish();
