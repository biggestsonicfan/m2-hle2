/*
 * grade-zsort.mjs — which face wins where faces lie on faces, this emulator's
 * pictures against MAME's.
 *
 * The board settles two faces in one plane with its polygon sort, a polygon at a
 * time; a depth buffer does it a pixel at a time and needs help (geo3d.h,
 * geo3d_mesh_layers). MAME's software renderer does what the board does, so its
 * picture is the answer. This grader plays attract's replay fight — the same
 * inputs on every board, so the same fight — in MAME and here, here twice (face
 * layers off, then on), and holds each picture against MAME's at the same frame.
 *
 * The replay is normally on Flying Carpet (stage 1, model 580's pyramid
 * shadows). --stage puts it elsewhere the same way in both emulators
 * (--match-replay-stage here, MR_STAGE in tools/mame/match-replay.lua): stage 5 is
 * Casino Night, where model 188's reel art stands 0.02 in front of its cabinet.
 *
 * The two renderers rasterise differently, so no frame matches to the pixel.
 * What is measured is the pixels the layers change: of every pixel where the
 * off and on pictures differ, how many does each put nearer MAME's. A frame is
 * only compared where both emulators drew it from the same camera, to within
 * --cam-tol (the eye and its angle at 0x519E98), since a fight that has drifted
 * is not the same picture. The replay drifts from MAME a few hundred frames in
 * even on its own stage, and sooner on another, so most frames drop out.
 *
 *   node tools/grade-zsort.mjs --mame [--stage 5]   # MAME's snapshots (~12 min)
 *   node tools/grade-zsort.mjs [--stage 5]          # play it here twice and grade
 *   node tools/grade-zsort.mjs --stage 1 --from 400 --to 1300 --step 30
 *   node tools/grade-zsort.mjs --stage 0 --toggle texclamp   # another set_camera switch
 *
 * --toggle NAME plays the replay with that set_camera switch at 0 and then at 1
 * in place of the face layers (which stay on in both), and grades the pixels it
 * changes the same way: texclamp (the filter's clamp at a tile edge) on South
 * Island's sky ring, zsort, zstanding.
 *
 * $MAME_EXE and $MAME_ROMPATH as tools/match-replay.mjs.
 */
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { REPO } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { readPng, writePng } from './lib/png.mjs';

const args = parseArgs(['stage', 'from', 'to', 'step', 'port', 'out', 'lag', 'tol', 'only-model', 'set', 'cam-tol', 'toggle']);
/* --toggle NAME: the set_camera switch the two plays differ by (see above). */
const TOGGLE = args.str('toggle', 'zlayers');
const WHAT = TOGGLE === 'zlayers' ? 'the layers' : TOGGLE;
/* --cam-tol D: grade frames whose camera eye is within D units of MAME's and whose angles are within
 * 64/65536 of a turn; 0 grades only bit-identical cameras. The off and on pictures share the camera, so
 * a small difference from MAME's adds the same noise to both. */
const CAM_TOL = args.num('cam-tol', 0.05);
/* --only-model N: the layers on only model N's faces, to find which model a result comes from. */
const ONLY = args.str('only-model', '-1');   /* N or LO-HI */
/* --set k=v,k=v: more set_camera settings for the play with the layers on (zlayer_steps, zlayer_board). */
const SET = Object.fromEntries((args.str('set', '') || '').split(',').filter(Boolean).map((kv) => kv.split('=')));
const STAGE = args.num('stage', 5);
const FROM = args.num('from', 400), TO = args.num('to', 1300), STEP = args.num('step', 30);
const OUT = path.resolve(args.str('out', path.join(os.tmpdir(), 'm2hle-zsort', `stage${STAGE}`)));
const W = 496, H = 384;
/* A pixel is "nearer MAME" by its largest channel difference. */
const TOL = args.num('tol', 24);
const CAMERA = 0x519e98, CAMERA_LEN = 0x10;
/* match-replay.lua's record, cut to what this needs: 16 bytes of each fighter
 * (it has to write something) and the camera. */
const MR_ROB = 0x10;
const REC = 8 + 2 * MR_ROB + CAMERA_LEN;
const rep = new Report(`grade-zsort — faces lying on faces, emulator vs MAME (stage ${STAGE}, replay frames ${FROM}..${TO} by ${STEP})`);

/* ---- MAME ------------------------------------------------------------------- */

if (args.bool('mame')) {
    const exe = process.env.MAME_EXE ?? path.resolve(REPO, '..', 'claude_mame', 'mame', 'mame.exe');
    const rompath = process.env.MAME_ROMPATH ?? path.join(REPO, 'tools', 'mame', 'mameroms');
    if (!fs.existsSync(exe)) { console.error(`no MAME at ${exe} — set $MAME_EXE`); process.exit(2); }
    for (const z of ['sfight.zip', 'schamp.zip', 'segabill.zip']) {
        if (!fs.existsSync(path.join(rompath, z))) { console.error(`${rompath} has no ${z} — set $MAME_ROMPATH`); process.exit(2); }
    }
    const stamp = String(Date.now());
    const dirs = { nvram: path.join(OUT, 'nvram-' + stamp), cfg: path.join(OUT, 'cfg-' + stamp), snap: path.join(OUT, 'mame') };
    for (const d of Object.values(dirs)) fs.mkdirSync(d, { recursive: true });
    const ref = path.join(OUT, 'mame.bin');
    rep.note(`running MAME headless to replay frame ${TO} -> ${dirs.snap}`);
    const t0 = Date.now();
    const r = spawnSync(exe, ['sfight', '-rompath', rompath, '-nvram_directory', dirs.nvram, '-cfg_directory', dirs.cfg,
        '-snapshot_directory', dirs.snap,
        '-nodrc', '-video', 'none', '-sound', 'none', '-nothrottle', '-skip_gameinfo', '-seconds_to_run', '299',
        '-autoboot_script', path.join(REPO, 'tools', 'mame', 'match-replay.lua')],
        { cwd: path.dirname(exe), stdio: 'ignore',
          env: { ...process.env, MR_OUT: ref, MR_FRAMES: String(TO + 1), MR_ROB: MR_ROB.toString(16),
                 MR_EXTRA: `${CAMERA.toString(16)}:${CAMERA_LEN.toString(16)}`,
                 MR_SNAP: `${FROM}:${TO}:${STEP}`, ...(STAGE !== 1 ? { MR_STAGE: String(STAGE) } : {}) } });
    const log = fs.existsSync(ref + '.log') ? fs.readFileSync(ref + '.log', 'utf8') : '';
    const shots = fs.readdirSync(dirs.snap).filter((f) => /^r\d+\.png$/.test(f)).length;
    rep.check('MAME played the replay and took its snapshots', /done/.test(log) && shots > 0,
              `${shots} snapshots in ${((Date.now() - t0) / 1000).toFixed(0)}s (exit ${r.status})`);
    if (STAGE !== 1) rep.check(`MAME put the replay on stage ${STAGE}`, (log.match(/substituted/g) ?? []).length === 2,
                               log.split(/\r?\n/).filter((l) => /substituted/.test(l)).join('; '));
    for (const d of [dirs.nvram, dirs.cfg]) fs.rmSync(d, { recursive: true, force: true });
    rep.finish();
    process.exit();
}

const refFile = path.join(OUT, 'mame.bin');
if (!fs.existsSync(refFile)) {
    rep.skip('the MAME reference is present', `no ${refFile} — take one with --mame --stage ${STAGE}`);
    rep.finish();
    process.exit();
}

const mame = [];
{
    const b = fs.readFileSync(refFile);
    for (let o = 0, n = 0; o + REC <= b.length; o += REC, n++)
        mame.push({ n, fc: b.readUInt32LE(o), stage: b[o + 4], camera: b.subarray(o + 8 + 2 * MR_ROB, o + REC) });
}

/* ---- here ------------------------------------------------------------------- */

/*
 * One play of the replay with face layers (or the --toggle switch) on or off. The picture of every
 * frame in reach of a MAME snapshot is kept, stamped with the board frame the
 * A/V stream gives it; capture_dl's marks give each board frame's
 * frame_counter and camera, which is how the two emulators' frames are paired.
 */
async function playHere(on, port) {
    const avPort = port + 100;
    const emu = await M2Hle.launch({ rom: findRom().primary, port, run: false,
        extraArgs: ['--match-replay-stage', String(STAGE), '--av-port', String(avPort), '--av-size', `${W}x${H}`] });
    const pictures = new Map();
    let jump = -1, sock = null;
    try {
        await emu.rpc('set_camera', TOGGLE === 'zlayers'
            ? { zlayers: String(on), zlayer_model: String(ONLY), ...(on ? SET : {}) }
            : { [TOGGLE]: String(on), ...(on ? SET : {}) });
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
                /* Near a snapshot: STEP apart, a few frames either side for the lag. */
                if (buf[0] === 0x56 && jump >= 0) {
                    const k = frame - jump - FROM;
                    const m = ((k % STEP) + STEP) % STEP;
                    if (k >= -8 && frame - jump <= TO + 8 && (m <= 8 || m >= STEP - 8))
                        pictures.set(frame, Buffer.from(buf.subarray(24, 24 + size)));
                }
                buf = buf.subarray(24 + size);
            }
        });
        await emu.waitForRom();
        await emu.run();
        for (const deadline = Date.now() + 120000; jump < 0;) {
            const st = await emu.status();
            if (st.match_replay === 'done') jump = st.match_replay_frame;
            else if (st.match_replay === 'unsupported') throw new Error('this profile has no attract replay');
            else if (Date.now() > deadline) throw new Error(`the replay jump never came (frame ${st.frames}, running ${st.running})`);
            else await new Promise((r) => setTimeout(r, 25));
        }
        const st = await emu.status();
        if (st.frames - jump >= FROM - 10) throw new Error(`reached frame ${st.frames} before the capture could start (jump at ${jump})`);
        const prefix = path.join(OUT, `here-${TOGGLE}${on}`);
        await emu.rpc('set_camera', {});   /* empties the list of layered models */
        const r = await emu.rpc('capture_dl', {
            frames: jump + TO + 12 - st.frames, path: prefix,
            probes: `500020:4,${[0, 4, 8, 12].map((o) => (CAMERA + o).toString(16) + ':4').join(',')}`,
            lo: 0x8cfff0, hi: 0x8d0000, max_words: 1024, timeout_ms: 900000,
        });
        if (!r.complete) throw new Error(`capture_dl stopped after ${r.frames} frames`);
        await new Promise((r2) => setTimeout(r2, 500));   /* the last pictures in flight */
        const models = (await emu.rpc('set_camera', {})).layer_models ?? [];
        const meta = JSON.parse(fs.readFileSync(prefix + '.json', 'utf8'));
        const marks = new Map(meta.marks.map((m) => {
            const cam = Buffer.alloc(CAMERA_LEN);
            for (let i = 0; i < 4; i++) cam.writeUInt32LE(m[3 + i] >>> 0, 4 * i);
            return [m[0], { fc: m[2] >>> 0, camera: cam }];
        }));
        for (const ext of ['.bin', '.json']) fs.rmSync(prefix + ext, { force: true });
        return { jump, pictures, marks, models };
    } finally {
        if (sock) sock.destroy();
        await emu.close();
    }
}

fs.mkdirSync(OUT, { recursive: true });
for (const f of fs.readdirSync(OUT)) if (/^compare-r\d+(-crop)?\.png$/.test(f)) fs.rmSync(path.join(OUT, f));
const port = args.num('port', 7520);
const [off, on] = [await playHere(0, port), await playHere(1, port + 2)];
rep.check('both plays jumped on the same board frame', off.jump === on.jump, `off ${off.jump}, on ${on.jump}`);
rep.note(`models drawn with layered faces: ${on.models.join(', ') || 'none'}${ONLY !== '-1' ? ` (layers on model ${ONLY} only)` : ''}`);

/* MAME's record n is the n-th frame edge after its jump; here that edge is
 * board frame jump + 1 + n. frame_counter counts from power-on and MAME shows
 * the boot warning this emulator skips, so the two differ by a constant; the
 * pairing is right when that constant is the same at every snapshot. */
const pairs = [];
let fcBad = 0, camBad = 0, fcDelta = null;
/* The eye (three floats) and the angle word (pitch low, yaw high, 0x10000 a turn). */
function camNear(a, b) {
    for (let k = 0; k < 12; k += 4) if (!(Math.abs(a.readFloatLE(k) - b.readFloatLE(k)) <= CAM_TOL)) return false;
    const ang = (x) => [x.readUInt16LE(12), x.readUInt16LE(14)];
    const [p0, y0] = ang(a), [p1, y1] = ang(b);
    const d = (u, v) => Math.min((u - v) & 0xffff, (v - u) & 0xffff);
    return d(p0, p1) <= 64 && d(y0, y1) <= 64;
}
for (const f of fs.readdirSync(path.join(OUT, 'mame')).filter((x) => /^r\d+\.png$/.test(x)).sort()) {
    const n = Number(f.slice(1, -4));
    const edge = off.jump + 1 + n, m = mame[n], h = off.marks.get(edge);
    if (!m || !h) continue;
    if (fcDelta === null) fcDelta = m.fc - h.fc;
    if (m.fc - h.fc !== fcDelta) { fcBad++; continue; }
    const onCam = on.marks.get(edge)?.camera;
    const exact = m.camera.equals(h.camera) && onCam?.equals(h.camera);
    const near = onCam?.equals(h.camera) && camNear(m.camera, h.camera);
    if (!exact) camBad++;
    pairs.push({ n, edge, file: path.join(OUT, 'mame', f), exact, sameCamera: CAM_TOL > 0 ? near : exact });
}
rep.check('every MAME snapshot pairs with a frame here by frame_counter', fcBad === 0 && pairs.length > 0,
          `${pairs.length} paired, MAME's frame_counter ${fcDelta} ahead (the boot warning), ${fcBad} off that`);
rep.note(`${pairs.length - camBad} of ${pairs.length} pairs have the camera bit-identical in both emulators, ${pairs.filter((p) => p.sameCamera).length} within ${CAM_TOL} units and 64/65536 of a turn; those are graded`);

/* The picture here that goes with MAME's snapshot at an edge: the renderer's
 * frame lags the edge by a fixed amount, found once by which lag agrees best. */
const bgra2rgb = (b) => { const o = Buffer.alloc(W * H * 3); for (let i = 0; i < W * H; i++) { o[3 * i] = b[4 * i + 2]; o[3 * i + 1] = b[4 * i + 1]; o[3 * i + 2] = b[4 * i]; } return o; };
function mae(a, b) { let s = 0; for (let i = 0; i < a.length; i++) s += Math.abs(a[i] - b[i]); return s / a.length; }
const graded = pairs.filter((p) => p.sameCamera);
for (const p of graded) p.mame = readPng(p.file).rgb;
let lag = args.num('lag', null);
if (lag === null) {
    let best = Infinity;
    for (let d = -4; d <= 4; d++) {
        let s = 0, k = 0;
        for (const p of graded) {
            const pic = on.pictures.get(p.edge + d);
            if (pic) { s += mae(bgra2rgb(pic), p.mame); k++; }
        }
        if (k >= Math.max(1, graded.length / 2) && s / k < best) { best = s / k; lag = d; }
    }
    rep.note(`picture lag ${lag} frames from the edge (mean abs difference ${best.toFixed(2)} a channel there)`);
}

/*
 * Per graded frame: the pixels where the layers change the picture, and of
 * those, how many each version puts nearer MAME (largest channel difference,
 * by more than TOL either way).
 */
let changed = 0, nearerOn = 0, nearerOff = 0, missing = 0;
const rows = [];
const px = (buf, i) => [buf[3 * i], buf[3 * i + 1], buf[3 * i + 2]];
const dist = (a, b) => Math.max(Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1]), Math.abs(a[2] - b[2]));
for (const p of graded) {
    const a = off.pictures.get(p.edge + lag), b = on.pictures.get(p.edge + lag);
    if (!a || !b) { missing++; continue; }
    const A = bgra2rgb(a), B = bgra2rgb(b);
    let c = 0, won = 0, lost = 0;
    for (let i = 0; i < W * H; i++) {
        const pa = px(A, i), pb = px(B, i);
        if (dist(pa, pb) <= TOL) continue;
        c++;
        const pm = px(p.mame, i), da = dist(pa, pm), db = dist(pb, pm);
        if (db + TOL < da) won++;
        else if (da + TOL < db) lost++;
    }
    changed += c; nearerOn += won; nearerOff += lost;
    rows.push({ n: p.n, changed: c, won, lost, A, B, M: p.mame });
}
if (missing) rep.note(`${missing} graded frames had no picture here at the lag (the stream dropped it)`);
for (const r of rows.filter((x) => x.changed).sort((x, y) => y.changed - x.changed).slice(0, 8))
    rep.note(`replay frame ${r.n}: ${r.changed} pixels changed by ${WHAT}, ${r.won} nearer MAME with them, ${r.lost} nearer without`);

/* The frame the layers change most, side by side: MAME | layers off | layers
 * on | what changed (green: nearer MAME with the layers, red: nearer without,
 * grey: neither), whole and then cropped to the changes and enlarged. */
const top = rows.reduce((x, y) => (y.changed > (x?.changed ?? -1) ? y : x), null);
if (top?.changed) {
    const map = Buffer.from(top.M);
    let x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (let i = 0; i < W * H; i++) {
        for (let k = 0; k < 3; k++) map[3 * i + k] >>= 2;
        const pa = px(top.A, i), pb = px(top.B, i);
        if (dist(pa, pb) <= TOL) continue;
        const pm = px(top.M, i), da = dist(pa, pm), db = dist(pb, pm);
        const c = db + TOL < da ? [0, 255, 0] : da + TOL < db ? [255, 0, 0] : [160, 160, 160];
        c.forEach((v, k) => { map[3 * i + k] = v; });
        const x = i % W, y = (i / W) | 0;
        x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y);
    }
    const panels = [top.M, top.A, top.B, map];
    const stem = path.join(OUT, `compare-r${String(top.n).padStart(5, '0')}`);
    const row = Buffer.alloc(W * panels.length * H * 3);
    for (let y = 0; y < H; y++) panels.forEach((src, k) => src.copy(row, (y * W * panels.length + k * W) * 3, y * W * 3, (y + 1) * W * 3));
    writePng(stem + '.png', { width: W * panels.length, height: H, rgb: row });
    const cw = x1 - x0 + 1, ch = y1 - y0 + 1, z = Math.max(1, Math.min(6, Math.floor(1600 / (cw * panels.length))));
    const crop = Buffer.alloc(cw * z * panels.length * ch * z * 3);
    for (let y = 0; y < ch * z; y++) for (let k = 0; k < panels.length; k++) for (let x = 0; x < cw * z; x++) {
        const s = ((y0 + ((y / z) | 0)) * W + x0 + ((x / z) | 0)) * 3, d = (y * cw * z * panels.length + k * cw * z + x) * 3;
        panels[k].copy(crop, d, s, s + 3);
    }
    writePng(stem + '-crop.png', { width: cw * z * panels.length, height: ch * z, rgb: crop });
    rep.note(`MAME | ${WHAT} off | ${WHAT} on | changes, replay frame ${top.n}: ${stem}.png (and -crop.png, x${z})`);
}

rep.check('graded frames', rows.length > 0, `${rows.length} frames`);
if (changed) {
    rep.check(`where ${WHAT} change the picture, they put it nearer MAME`, nearerOn > nearerOff,
              `${changed} pixels changed: ${nearerOn} nearer MAME with ${WHAT}, ${nearerOff} nearer without, ` +
              `${changed - nearerOn - nearerOff} no nearer either way`);
} else {
    rep.note(`${WHAT} changed no pixel on the graded frames`);
}
rep.finish();
