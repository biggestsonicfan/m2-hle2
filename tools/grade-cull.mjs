/*
 * grade-cull.mjs — which arena ground chunks this emulator draws, against the
 * ones the board's own cull would draw from the same camera.
 *
 * The explorer draws every part of an arena, because its camera can be
 * anywhere; the board draws the subset its camera can see. verify-stage.mjs
 * therefore calls a part the board did not draw "culled" and moves on. This is
 * the other half: given the camera the display list says the frame was drawn
 * from, is the subset the right one?
 *
 * For the sixteen ground chunks that is a question with an exact answer,
 * because the cull is the ROM's own arithmetic on ROM tables:
 *
 *   ground_disp pushes the stage frame, then clip_point_check_yoko (0x28188)
 *   runs a 5x5 lattice of ground points (0x90654: x, z at -200..200 step 100)
 *   through op 0x29 and writes one outcode byte per point to 0x50E000:
 *
 *       z <= 1                     0x90   behind the lens
 *       x*f*2/3 / z <= -248        0x81   left of the screen
 *       x*f*2/3 / z >=  248        0x82   right of it
 *
 *   with f = focus_dist_x (0x501084). area_clip (0x28334) then walks
 *   area_clip_data (0x904D0) — four lattice indices per chunk — and draws a
 *   chunk only when the AND of its four outcodes is zero. Bit 7 rides on every
 *   outside code, so a chunk survives only while one of its corners is on
 *   screen. The chunk the camera stands over would then vanish whenever all
 *   four corners are outside it, so last of all the routine takes the camera's
 *   position in the stage frame (op 0x6A of the origin), picks its 4x4 cell
 *   with (p + 320) / 160, and draws that cell's chunk if the loop did not.
 *
 * So the prediction needs only the matrix ground_disp tested with, which the
 * display list itself carries, and focus_dist_x, which the capture probes.
 * Nothing the emulator computed about visibility goes into it. The explorer is
 * the oracle for everything else: which models are this stage's ground chunks,
 * in which slots, and the matrix each one should be drawn at.
 *
 *   node tools/grade-cull.mjs                    # capture a fight, then grade it
 *   node tools/grade-cull.mjs --capture <prefix> [--stage N]
 *   node tools/grade-cull.mjs --frames 600 --detail
 */
import fs from 'node:fs';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom, loadRom } from './lib/rom.mjs';
import { nc } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { captureFight, PROBES, DEFAULT_DL_OUT, COPRO_FIFO, segment, f32 } from './lib/dl.mjs';

const args = parseArgs(['capture', 'stage', 'frames', 'port', 'out']);
const rep = new Report('grade-cull — ground chunks drawn, emulator vs the board\'s area_clip');

/* ---- ROM tables the cull reads -------------------------------------------- */

const CULL_GRID = 0x90654;        /* dword_90654: count, then (x, z) float pairs */
const AREA_CLIP_DATA = 0x904d0;   /* count - 1, then four lattice indices a chunk */
const STAGE_DATA = 0x8f3d0;
const STAGE_PARTS = 0x64;
const FOCUS_DIST_X = 0x501084;
const FOCUS_DIST_Y = 0x501088;
const TWO_THIRDS = 0x3f2aacda;    /* lda 0x3F2AACDA, then mulr: the bits are the float */
const SCALE_16 = 0x3fcccccd;

const PROBE_LIST = [...PROBES, ['focusX', FOCUS_DIST_X, 4], ['focusY', FOCUS_DIST_Y, 4]];

const OP = { PUSH: 0x01, POP: 0x02, TRANSLATE: 0x06, SCALE: 0x07, ANG_X: 0x08, ANG_Y: 0x09,
             ANG_Z: 0x0a, POINT: 0x29, GLO_TO_LOC: 0x6a, DRAW: 0x78 };

/* ---- the capture ------------------------------------------------------------ */

let prefix = args.str('capture');
let stage = args.num('stage', null);
if (!prefix) {
    const emu = await M2Hle.launch({ rom: findRom().primary, port: args.num('port', 7172) });
    try {
        await emu.waitUntilRunning();
        const cap = await captureFight(emu, {
            out: args.str('out', path.join(path.dirname(DEFAULT_DL_OUT), 'm2hle-cull')),
            frames: args.num('frames', 300), probes: PROBE_LIST, log: (s) => rep.note(s),
        });
        prefix = cap.prefix;
        stage ??= cap.scene.stage;
        rep.note(`captured ${cap.frames} frames, ${cap.words} words -> ${prefix}`);
    } finally {
        await emu.close();
    }
}
if (stage === null) {
    const scene = path.join(path.dirname(prefix), 'fight-scene.json');
    if (fs.existsSync(scene)) stage = JSON.parse(fs.readFileSync(scene, 'utf8')).stage;
}

/* ---- matrices: row-major, column vectors, the board's rotation sense -------
 * The same conventions stf-tools/dl-verify.mjs replays MAME's display lists
 * with, which verify-stage.mjs holds to the board's own draws. */

const I4 = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
function mul(a, b) {
    const o = new Array(16);
    for (let r = 0; r < 4; r++) {
        for (let c = 0; c < 4; c++) {
            let s = 0;
            for (let k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
    }
    return o;
}
const scaleM = (x, y, z) => [x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1];
const transM = (x, y, z) => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1];
const ang = (w) => { const v = w & 0xffff; return (v >= 0x8000 ? v - 0x10000 : v) * Math.PI / 32768; };
const rotY = (t) => { const c = Math.cos(t), s = Math.sin(t); return [c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1]; };
const rotX = (t) => { const c = Math.cos(t), s = Math.sin(t); return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1]; };
const rotZ = (t) => { const c = Math.cos(t), s = Math.sin(t); return [c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; };
const maxdiff = (a, b) => a.reduce((m, v, i) => Math.max(m, Math.abs(v - b[i])), 0);

/* ---- the board's cull, ported ---------------------------------------------- */

const { rom } = await loadRom();
const { readStageTable } = await nc('stages.js');
const dv = rom.mainCpuView;

const grid = [];
for (let i = 0, n = dv.getUint32(CULL_GRID, true); i < n; i++) {
    grid.push([dv.getFloat32(CULL_GRID + 4 + i * 8, true), dv.getFloat32(CULL_GRID + 8 + i * 8, true)]);
}
const corners = [];
for (let i = 0, n = dv.getUint16(AREA_CLIP_DATA, true) + 1; i < n; i++) {
    corners.push([0, 1, 2, 3].map((k) => dv.getUint16(AREA_CLIP_DATA + 2 + (i * 4 + k) * 2, true)));
}
const fr32 = Math.fround;

/**
 * clip_point_check_yoko's outcodes for the lattice under matrix Y, plus how
 * close the nearest decision came to going the other way (in screen pixels, and
 * in eye-space z against the lens at 1.0) — a prediction that close is a
 * float32-versus-float64 coin toss and is reported, not failed.
 */
function outcodes(Y, focusX) {
    const f = fr32(f32(focusX) * f32(TWO_THIRDS));
    let margin = Infinity;
    const codes = grid.map(([x, z]) => {
        const ex = fr32(Y[0] * x + Y[2] * z + Y[3]);
        const ez = fr32(Y[8] * x + Y[10] * z + Y[11]);
        margin = Math.min(margin, Math.abs(ez - 1) * 100);
        if (!(ez > 1)) return 0x90;
        const sx = fr32(fr32(ex * f) / ez);
        margin = Math.min(margin, Math.abs(sx + 248), Math.abs(sx - 248));
        let c = 0;
        if (!(sx > -248)) c |= 0x81;
        if (!(sx < 248)) c |= 0x82;
        return c;
    });
    return { codes, margin };
}

/** area_clip's draw list: chunk slots in the order the routine draws them. */
function areaClip(Y, focusX, parts) {
    const { codes, margin } = outcodes(Y, focusX);
    const slots = [];
    let mask = 0;
    corners.forEach((cn, i) => {
        if ((codes[cn[0]] & codes[cn[1]] & codes[cn[2]] & codes[cn[3]]) !== 0) return;
        if (parts[i] === 0) return;
        slots.push(i);
        mask |= 1 << i;
    });
    /* op 0x6A of the origin under Y * S(1.6): rot^T * (0 - T), the rotation
     * still carrying its 1.6 — Fn_glo_to_loc does not divide it out. */
    const M = mul(Y, scaleM(1.6, 1.6, 1.6));
    const T = [M[3], M[7], M[11]];
    const loc = (c) => -(M[c] * T[0] + M[4 + c] * T[1] + M[8 + c] * T[2]);
    const cell = (v) => {
        const i = Math.round(fr32(v + 320)) | 0;           /* cvtri, round to nearest */
        return Math.floor((i >>> 0) / 160) & 3;            /* divo: unsigned */
    };
    const under = cell(loc(2)) * 4 + cell(loc(0));
    let cameraCell = null;
    if (!(mask & (1 << under))) { slots.push(under); cameraCell = under; }
    return { slots, codes, margin, under, cameraCell };
}

/* ---- the coprocessor's current matrix, replayed ----------------------------
 *
 * The cull depends on the absolute camera, not on one recovered relative to
 * the draws the way verify-stage.mjs recovers it, so this replay has to be the
 * coprocessor's own matrix. Two things make that so:
 *
 *   - it runs over the whole capture without resetting at frame marks. The
 *     marks fall where the frame hook fires, which is not where camera_init
 *     starts: a frame slice opens with the view already on the stack;
 *   - it applies every op that sets the current matrix outright, not only the
 *     push/pop/translate/scale/angle ops a stage draw is built from. Fn_base_matrix
 *     (0x03) is how camera_init starts the view; without it the replay was off
 *     from the board by a half turn about Z.
 *
 * The ops that read the unit-matrix caches (0x36, 0x37, 0x46, 0x7E) are left
 * out: they build the fighters inside a push/pop, and the pop restores the
 * matrix the stage passes need. Held against the emulator's own coprocessor at
 * ground_disp over 40 frames of a fight, this agrees to 2e-4.
 */
function affineInverse(m) {
    const a = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]];
    const d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6])
            + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (Math.abs(d) < 1e-20) return m;
    const c = [
        (a[4] * a[8] - a[5] * a[7]) / d, -(a[1] * a[8] - a[2] * a[7]) / d, (a[1] * a[5] - a[2] * a[4]) / d,
        -(a[3] * a[8] - a[5] * a[6]) / d, (a[0] * a[8] - a[2] * a[6]) / d, -(a[0] * a[5] - a[2] * a[3]) / d,
        (a[3] * a[7] - a[4] * a[6]) / d, -(a[0] * a[7] - a[1] * a[6]) / d, (a[0] * a[4] - a[1] * a[3]) / d,
    ];
    const t = [m[3], m[7], m[11]];
    return [c[0], c[1], c[2], -(c[0] * t[0] + c[1] * t[1] + c[2] * t[2]),
            c[3], c[4], c[5], -(c[3] * t[0] + c[4] * t[1] + c[5] * t[2]),
            c[6], c[7], c[8], -(c[6] * t[0] + c[7] * t[1] + c[8] * t[2]), 0, 0, 0, 1];
}
/* 12 floats as the coprocessor holds them: three columns, then T. */
const colsM = (F) => [F[0], F[3], F[6], F[9], F[1], F[4], F[7], F[10], F[2], F[5], F[8], F[11], 0, 0, 0, 1];

class Replay {
    constructor() { this.m = I4(); this.stack = []; }
    apply(c) {
        const a = c.args;
        let m = this.m;
        switch (c.op) {
            case OP.PUSH: this.stack.push(m.slice()); break;
            case OP.POP: if (this.stack.length) m = this.stack.pop(); break;
            case 0x03: m = I4(); break;                                        /* Fn_base_matrix */
            case 0x04: m = colsM(a.map(f32)); break;                           /* Fn_load_matrix */
            case 0x0b: m = mul(colsM(a.map(f32)), m); break;                   /* Fn_mul_matrix: M * current */
            case 0x0c: m = affineInverse(m); break;                            /* Fn_inv_matrix */
            case 0x0d: m = m.slice(); m[3] = m[7] = m[11] = 0; break;          /* Fn_base_point */
            case 0x0e: m = m.slice(); [m[3], m[7], m[11]] = a.map(f32); break; /* Fn_load_point */
            case 0x10: m = [1, 0, 0, m[3], 0, 1, 0, m[7], 0, 0, 1, m[11], 0, 0, 0, 1]; break; /* Fn_base_3x3 */
            case OP.TRANSLATE: m = mul(m, transM(f32(a[0]), f32(a[1]), f32(a[2]))); break;
            case OP.SCALE: m = mul(m, scaleM(f32(a[0]), f32(a[1]), f32(a[2]))); break;
            case OP.ANG_X: m = mul(m, rotX(ang(a[0]))); break;
            case OP.ANG_Y: m = mul(m, rotY(ang(a[0]))); break;
            case OP.ANG_Z: m = mul(m, rotZ(ang(a[0]))); break;
            case 0x3f: m = mul(mul(mul(m, rotZ(ang(a[0]))), rotY(ang(a[1]))), rotX(ang(a[2]))); break; /* Fn_zyx_rot */
            default: break;
        }
        this.m = m;
    }
}

/* ---- finding ground_disp's pass in a frame --------------------------------- */

/**
 * Walk a frame's coprocessor commands through the replay and pick out
 * ground_disp: push, the stage frame's three angles and translation,
 * clip_point_check_yoko's lattice (when it ran), then area_clip under a pushed
 * 1.6 scale, closed by its pop.
 */
function groundPass(cmds, replay) {
    const after = cmds.map((c) => { replay.apply(c); return replay.m; });
    const ops = cmds.map((c) => c.op);
    for (let i = 0; i + 6 < cmds.length; i++) {
        if (ops[i] !== OP.PUSH || ops[i + 1] !== OP.ANG_Z || ops[i + 2] !== OP.ANG_X
            || ops[i + 3] !== OP.ANG_Y || ops[i + 4] !== OP.TRANSLATE) continue;
        let j = i + 5;
        const lattice = [];
        while (j < cmds.length && ops[j] === OP.POINT) lattice.push(cmds[j++].args);
        if (ops[j] !== OP.PUSH || ops[j + 1] !== OP.SCALE
            || !cmds[j + 1].args.every((w) => w === SCALE_16)) continue;
        const draws = [];
        let depth = 0, glo = false;
        for (let k = j + 2; k < cmds.length; k++) {
            if (ops[k] === OP.PUSH) depth++;
            else if (ops[k] === OP.POP) { if (depth-- === 0) break; }
            else if (ops[k] === OP.GLO_TO_LOC) glo = true;
            else if (ops[k] === OP.DRAW && depth === 0) {
                draws.push({ args: cmds[k].args, m: after[k], afterGlo: glo });
            }
        }
        /* The view is the matrix ground_disp pushed over: the one standing
         * before its first op. */
        return { Y: after[i + 4], C: i > 0 ? after[i - 1] : null, lattice, draws };
    }
    return null;
}

/* ---- reading the capture ---------------------------------------------------- */

const meta = JSON.parse(fs.readFileSync(`${prefix}.json`, 'utf8'));
const bin = fs.readFileSync(`${prefix}.bin`);
const named = (mk) => Object.fromEntries(PROBE_LIST.map(([n], i) => [n, mk[2 + i]]));
if ((meta.marks[0]?.length ?? 0) - 2 !== PROBE_LIST.length) {
    rep.skip('ground chunk cull', `${prefix} was not captured with this grader's probes (focus_dist_x) — capture again`);
    rep.finish();
}
if (stage === null) stage = named(meta.marks[0]).stageNum;

const record = readStageTable(rom)[stage];
const base = STAGE_DATA + stage * 256 + STAGE_PARTS;
const parts = Array.from({ length: 16 }, (_, i) => dv.getUint16(base + i * 2, true));
rep.note(`stage ${stage} (${record.name}), explorer ground layer: ${record.layers.ground.join(' ')}`);
rep.check('the explorer\'s ground layer is the record\'s 16 area_clip slots',
          JSON.stringify(parts.filter((p) => p !== 0)) === JSON.stringify(record.layers.ground),
          `slots ${parts.join(' ')}`);
if (record.flags & (1 << 13)) {
    rep.skip('ground chunk cull', `stage ${stage} sets flag bit 13, so ground_disp never runs area_clip`);
    rep.finish();
}

const modelOf = new Map();
for (let i = 0; i < 5103; i++) {
    const o = 0xe0004 + i * 16;
    const k = `${rom.mainDataView.getUint32(o, true)}/${rom.mainDataView.getUint32(o + 4, true)}/${rom.mainDataView.getUint32(o + 8, true)}`;
    if (!modelOf.has(k)) modelOf.set(k, i);
}
const drawModel = (d) => modelOf.get(`${d.args[2]}/${d.args[3]}/${d.args[4]}`) ?? -1;
/* On a stage that moves, ground_disp's frame is not the identity and Y is not C. */
const { stageWorldFrame, readFrameTables } = await nc('display.js');
const moving = stageWorldFrame(record, 0, readFrameTables(rom)) !== null;

/* ---- grading ----------------------------------------------------------------- */

/* One stream for the whole capture, segmented once, so a command whose
 * arguments straddle a frame mark still reads as one command. Each command is
 * filed under the frame its command word was written in. */
const copro = [], wordAt = [];
for (let k = 0; k * 8 < bin.length; k++) {
    const o = bin.readUInt32LE(k * 8);
    if (o >= COPRO_FIFO[0] && o < COPRO_FIFO[1]) { copro.push(bin.readUInt32LE(k * 8 + 4)); wordAt.push(k); }
}
const frameCmds = meta.marks.map(() => []);
{
    let f = 0;
    for (const c of segment(copro)) {
        const k = wordAt[c.at ?? 0];
        while (f + 1 < meta.marks.length && k >= meta.marks[f + 1][1]) f++;
        frameCmds[f].push(c);
    }
}

const S16 = scaleM(1.6, 1.6, 1.6);
const replay = new Replay();
let found = 0, ran = 0, exact = 0, near = 0, worstM = 0, worstYC = 0, cellHits = 0;
const bad = [];
const toggles = { emu: 0, board: 0 };
let prevEmu = null, prevBoard = null;
for (let f = 0; f + 1 < meta.marks.length; f++) {
    const a = meta.marks[f], b = meta.marks[f + 1];
    const pass = groundPass(frameCmds[f], replay);
    /* The first frame opens with the stack the capture walked in on; the
     * replay only holds the coprocessor's matrix once camera_init has run. */
    if (!pass || f === 0) continue;
    found++;
    if (pass.lattice.length === grid.length) ran++;
    const probes = named(b);
    const want = areaClip(pass.Y, probes.focusX, parts);
    const got = pass.draws.map(drawModel);
    const wantModels = want.slots.map((s) => parts[s]);
    for (const d of pass.draws) worstM = Math.max(worstM, maxdiff(d.m, mul(pass.Y, S16)));
    if (!moving) worstYC = Math.max(worstYC, maxdiff(pass.Y, pass.C));
    if (want.cameraCell !== null) cellHits++;

    const same = JSON.stringify(got) === JSON.stringify(wantModels);
    if (same) exact++;
    else if (want.margin < 0.01) near++;
    else bad.push({ frame: a[0], got, want: wantModels, want_slots: want.slots, codes: want.codes,
                    under: want.under, focus: f32(probes.focusX), lattice: pass.lattice.length });

    const gs = new Set(got), ws = new Set(wantModels);
    if (prevEmu) {
        for (const p of new Set([...gs, ...prevEmu])) if (gs.has(p) !== prevEmu.has(p)) toggles.emu++;
        for (const p of new Set([...ws, ...prevBoard])) if (ws.has(p) !== prevBoard.has(p)) toggles.board++;
    }
    prevEmu = gs; prevBoard = ws;
}
const frames = meta.marks.length - 1;

rep.check('ground_disp\'s pass is in the display list', found > 0,
          `${found} of ${frames - 1} frames (the first only primes the replay)`);
if (!found) rep.finish();
rep.note(`clip_point_check_yoko sent its ${grid.length} lattice points in ${ran} of ${found} frames`);
rep.check('every chunk is drawn at the explorer\'s matrix (Y * S(1.6))', worstM < 1e-3,
          `worst ${worstM.toExponential(2)}`);
if (!moving) {
    rep.check('ground_disp tests with the view matrix (static stage: Y = C)', worstYC < 1e-3,
              `worst ${worstYC.toExponential(2)}`);
}
rep.check('the chunks drawn are the ones area_clip selects', bad.length === 0,
          `${exact} of ${found} frames exact, ${bad.length} wrong, ${near} too close to call; ` +
          `camera-cell fallback drew in ${cellHits}`);
rep.note(`chunks switching on/off between frames: emulator ${toggles.emu}, board's rule ${toggles.board}`);
for (const x of bad.slice(0, args.bool('detail') ? 40 : 6)) {
    const extra = x.got.filter((m) => !x.want.includes(m));
    const missing = x.want.filter((m) => !x.got.includes(m));
    rep.note(`  frame ${x.frame}: drew ${x.got.length}, rule selects ${x.want.length}` +
             (extra.length ? `; extra ${extra.join(' ')}` : '') +
             (missing.length ? `; missing ${missing.join(' ')}` : '') +
             (extra.length || missing.length ? '' : '; same set, different order') +
             `  (lattice ${x.lattice}, focus ${x.focus.toFixed(1)}, camera over slot ${x.under})`);
    if (args.bool('detail')) {
        rep.note(`    outcodes ${x.codes.map((c) => c.toString(16).padStart(2, '0')).join(' ')}`);
    }
}
rep.finish();
