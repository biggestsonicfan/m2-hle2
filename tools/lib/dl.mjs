/*
 * dl.mjs — the display list, taken off a running emulator.
 *
 * The explorer toolkit's MAME tap (stf-tools/mame-dl-capture.lua) records every
 * word the i960 writes to the geometry processor and the coprocessor, in write
 * order, with a mark at each frame edge carrying a handful of game variables.
 * The capture_dl bridge command records the same thing out of this emulator,
 * so the toolkit's display-list checks read either one. This file owns the
 * probes, the fight a capture is taken in, and the two layouts those checks
 * expect.
 *
 * What it writes is the game's own data. It goes to a temp directory outside
 * the checkout by default, as board captures do.
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {
    FRAME_COUNTER, STAGE_NUM, P1_ROB, P2_ROB, ROB_MOTION, ROB_COMA, ROB_CHAR, ROB_SKELETON,
    ROB_STATE, ROB_STATE_MIRROR,
    ROB_FLAGS, ROB_SMOOTH_MODE, ROB_SMOOTH_IN, ROB_SMOOTH_OUT_AT, ROB_SMOOTH_COUNT,
    ROB_MOTION_LENGTH, ROB_TIMEWARP, NOT_SCR_BG_MOVE, REPLAY_COUNTDOWN,
    CARPET_ANG_X, CARPET_HEADING, CARPET_ROLL, SKY_ANGLE,
} from './board.mjs';
import { identifyScene } from './capture.mjs';

export const DEFAULT_DL_OUT = path.join(os.tmpdir(), 'm2hle-fight');

/* The two ports, as stf-tools/dl-verify.mjs names them. */
export const COPRO_FIFO = [0x884000, 0x888000];
export const GEO_FIFO = [0x804000, 0x808000];

/* Read at every frame edge, in this order. The names are what the rest of this
 * file and the graders index them by. */
export const PROBES = [
    ['carpetAngX',    CARPET_ANG_X, 2],
    ['carpetHeading', CARPET_HEADING, 2],
    ['carpetRoll',    CARPET_ROLL, 2],
    ['skyAngle',      SKY_ANGLE, 2],
    ['stageNum',      STAGE_NUM, 1],
    ['frameCounter',  FRAME_COUNTER, 4],
    ['notScrBgMove',  NOT_SCR_BG_MOVE, 4],
    ['replayCountdown', REPLAY_COUNTDOWN, 2],
    ...[P1_ROB, P2_ROB].flatMap((base, p) => [
        [`p${p + 1}Motion`,   base + ROB_MOTION, 2],
        [`p${p + 1}Coma`,     base + ROB_COMA, 2],
        [`p${p + 1}Char`,     base + ROB_CHAR, 1],
        [`p${p + 1}Skeleton`, base + ROB_SKELETON, 1],
        [`p${p + 1}Flags`,       base + ROB_FLAGS, 4],
        [`p${p + 1}SmoothMode`,  base + ROB_SMOOTH_MODE, 1],
        [`p${p + 1}SmoothIn`,    base + ROB_SMOOTH_IN, 2],
        [`p${p + 1}SmoothOutAt`, base + ROB_SMOOTH_OUT_AT, 2],
        [`p${p + 1}SmoothCount`, base + ROB_SMOOTH_COUNT, 2],
        [`p${p + 1}Length`,      base + ROB_MOTION_LENGTH, 2],
        [`p${p + 1}Timewarp`,    base + ROB_TIMEWARP, 4],
        [`p${p + 1}State`,       base + ROB_STATE, 4],
    ]),
];

/* PROBES plus the scene: the camera struct camera_init turns into the view
 * (eye, pitch/yaw/roll, mode, zoom and the load_point offset), each fighter's
 * world placement (position, the three angles, the +0x80 offset rob_disp
 * translates by, +0x678 load_point Y, +0x27C0 part Y scale, the +0x198 command
 * request) and the attract movie's counters. Floats are read as their bits. */
const CAMERA = 0x519e80;
export const SCENE_PROBES = [
    ...PROBES,
    ['camEyeX', CAMERA + 0x18, 4], ['camEyeY', CAMERA + 0x1c, 4], ['camEyeZ', CAMERA + 0x20, 4],
    ['camPitch', CAMERA + 0x24, 2], ['camYaw', CAMERA + 0x26, 2], ['camRoll', CAMERA + 0x28, 2],
    ['camMode', CAMERA + 0x40, 4], ['camZoom', CAMERA + 0x13c, 4], ['camPoint', CAMERA + 0x140, 4],
    ...[P1_ROB, P2_ROB].flatMap((base, p) => [
        [`p${p + 1}PosX`, base + 0x18, 4], [`p${p + 1}PosY`, base + 0x1c, 4], [`p${p + 1}PosZ`, base + 0x20, 4],
        [`p${p + 1}AngX`, base + 0x24, 2], [`p${p + 1}AngY`, base + 0x26, 2], [`p${p + 1}AngZ`, base + 0x28, 2],
        [`p${p + 1}OffX`, base + 0x80, 4], [`p${p + 1}OffY`, base + 0x84, 4], [`p${p + 1}OffZ`, base + 0x88, 4],
        [`p${p + 1}Request`, base + 0x198, 2],
        [`p${p + 1}PointY`, base + 0x678, 4],
        [`p${p + 1}PartScaleY`, base + 0x27c0, 4],
    ]),
    ['amCntr', 0x5004c4, 4], ['amFlags', 0x5004c8, 4], ['amCont', 0x5004cc, 4],
];
export const probeListSpec = (list) => list.map(([, a, s]) => `${a.toString(16)}:${s}`).join(',');

/*
 * set_mirror (0x30D0C), as it rewrites the sampled pose: a motion played
 * mirrored swaps each left/right pair of objects and reflects the pose across
 * the body's own plane. Written in the sample's channel numbering — the i960's
 * buffer holds one channel per 4 bytes, angle objects 0..11 then float objects
 * 12..19, three axes each — so each entry reads straight off an offset the
 * routine touches (offset / 4).
 *
 *   neg      an angle or float negated: subi from 0, or notbit 31 on a float
 *   flip     an angle reflected about 0x8000 (a yaw seen from the other side)
 *   swap     two channels exchanged
 *   swapNeg  exchanged and both negated
 */
const MIRROR = {
    neg: [13, 14, 36, 39, 42],
    flip: [15, 17, 27, 29],
    swapNeg: [[0, 3], [1, 4], [21, 24], [22, 25], [30, 33], [31, 34], [7, 10], [8, 11],
              [45, 48], [54, 57]],
    swap: [[2, 5], [23, 26], [32, 35], [6, 9], [46, 49], [47, 50], [55, 58], [56, 59]],
};

/** A pose sampleMotion() returned, as the board has it after set_mirror. */
export function mirrorSample(s) {
    const angles = Uint16Array.from(s.angles);
    const targets = Float32Array.from(s.targets);
    const get = (c) => (c < 36 ? angles[c] : targets[c - 36]);
    /* An angle wraps to 16 bits on the way in, so negating one is -v mod 0x10000. */
    const set = (c, v) => { if (c < 36) angles[c] = v & 0xffff; else targets[c - 36] = v; };
    for (const c of MIRROR.neg) set(c, -get(c));
    for (const c of MIRROR.flip) set(c, 0x8000 - get(c));
    for (const [a, b] of MIRROR.swapNeg) { const va = get(a), vb = get(b); set(a, -vb); set(b, -va); }
    for (const [a, b] of MIRROR.swap) { const va = get(a), vb = get(b); set(a, vb); set(b, va); }
    return { ...s, angles, targets };
}

/** Whether this fighter's motion was played mirrored on the frame a mark closes. */
export const mirroredOf = (probes, p) => Boolean(probes[p + 'State'] & ROB_STATE_MIRROR);

/**
 * The frame get_frame_dat (0x304C8) samples this fighter's motion at, which is
 * not always the motion frame:
 *
 *   - a replay (bit 17 of not_scr_bg_move and bit 0 of replay_countdown)
 *     samples half a frame on, except on the motion's last frame;
 *   - a motion with a time-warp table remaps the frame through its
 *     (frame, sampled frame) byte pairs, linearly between the two keys either
 *     side. On a frame that *is* a key the routine takes the frame as it is,
 *     not the key's value — it compares before it reads the value byte — and
 *     this follows it.
 *
 * `readByte(addr)` reads the ROM at an i960 address. Returns the frame, which
 * may be fractional.
 */
export function sampledFrame(probes, p, readByte) {
    const coma = probes[p + 'Coma'];
    if ((probes.notScrBgMove & 0x20000) && (probes.replayCountdown & 1) && coma !== probes[p + 'Length']) {
        return coma + 0.5;
    }
    let table = probes[p + 'Timewarp'] >>> 0;
    if (!table) return coma;
    let key = 0, value = 0;
    for (let guard = 0; guard < 256; guard++, table += 2) {
        const prevKey = key, prevValue = value;
        key = readByte(table);
        if (key === coma) return coma;
        value = readByte(table + 1);
        if (key > coma) {
            return (prevValue * (key - prevKey) + (value - prevValue) * (coma - prevKey)) / (key - prevKey);
        }
    }
    return coma;
}

/**
 * Whether get_frame_dat blended this fighter's pose on the frame a mark closes,
 * read the way the routine decides it (0x30580..0x30640): the ease-in while
 * its count is under the ease length, the ease-out once the motion frame is
 * past its start. `p` is 'p1' or 'p2'; `probes` a mark's named probes.
 */
export function blendOf(probes, p) {
    const mode = probes[p + 'SmoothMode'];
    const coma = probes[p + 'Coma'];
    const count = (probes[p + 'Flags'] & 0x20010) ? probes[p + 'SmoothCount'] : coma;
    if (mode === 0) return null;
    if (mode !== 2 && count < probes[p + 'SmoothIn']) return 'in';
    if (mode !== 1 && coma > probes[p + 'SmoothOutAt']) return 'out';
    return null;
}

const probeSpec = () => PROBES.map(([, a, s]) => `${a.toString(16)}:${s}`).join(',');

/* ---- reading a capture ---------------------------------------------------- */

const f32buf = new DataView(new ArrayBuffer(4));
export function f32(w) { f32buf.setUint32(0, w >>> 0, true); return f32buf.getFloat32(0, true); }

export const isCmd = (w) => {
    const op = w & 0xff;
    return op !== 0 && w === ((((op << 23) >>> 0) | (op << 8) | op) >>> 0);
};

/** Split a run of coprocessor words into {op, args}; the command words name themselves. */
export function segment(words) {
    const out = [];
    for (let i = 0; i < words.length; i++) {
        if (!isCmd(words[i])) continue;
        const op = words[i] & 0xff;
        const args = [];
        let j = i + 1;
        while (j < words.length && !isCmd(words[j])) args.push(words[j++]);
        out.push({ op, args, at: i });
        i = j - 1;
    }
    return out;
}

/**
 * Load a capture as frames. Each frame is the words between two marks, with
 * the probes read at the mark that *closes* it — the game state the commands in
 * it were computed from, which is how the MAME tooling labels a slice too.
 */
export function loadDl(prefix) {
    const bin = fs.readFileSync(`${prefix}.bin`);
    const meta = JSON.parse(fs.readFileSync(`${prefix}.json`, 'utf8'));
    const n = bin.length / 8;
    const offs = new Uint32Array(n), words = new Uint32Array(n);
    for (let i = 0; i < n; i++) {
        offs[i] = bin.readUInt32LE(i * 8);
        words[i] = bin.readUInt32LE(i * 8 + 4);
    }
    /* Whichever probe list the capture was taken with, told apart by its length. */
    const list = (meta.marks[0]?.length ?? 0) - 2 === SCENE_PROBES.length ? SCENE_PROBES : PROBES;
    const named = (m) => Object.fromEntries(list.map(([name], i) => [name, m[2 + i]]));
    const frames = [];
    for (let i = 0; i + 1 < meta.marks.length; i++) {
        const a = meta.marks[i], b = meta.marks[i + 1];
        const copro = [];
        for (let k = a[1]; k < b[1]; k++) {
            if (offs[k] >= COPRO_FIFO[0] && offs[k] < COPRO_FIFO[1]) copro.push(words[k]);
        }
        frames.push({ frame: a[0], opened: named(a), closed: named(b), copro });
    }
    return { meta, offs, words, frames };
}

/* ---- taking one ----------------------------------------------------------- */

/**
 * Record `frames` whole frames to `<prefix>.bin/.json`. `range` limits what is
 * recorded to one port — COPRO_FIFO is all the rig needs, and a fraction of the
 * words — and defaults to both, which placement needs.
 */
export async function captureDl(emu, prefix, frames, {
    range = null, tgp = false, scene = false, slots = false, unit = false, probes = null,
} = {}) {
    fs.mkdirSync(path.dirname(path.resolve(prefix)), { recursive: true });
    const r = await emu.rpc('capture_dl', {
        frames, path: path.resolve(prefix),
        probes: probes ? probeListSpec(probes) : scene ? probeListSpec(SCENE_PROBES) : probeSpec(),
        ...(range ? { lo: range[0], hi: range[1] } : {}),
        ...(tgp ? { tgp: 1 } : {}),
        ...(slots ? { slots: 1 } : {}),
        ...(unit ? { unit: 1 } : {}),
        /* A busy frame is a few thousand words on the coprocessor port alone. */
        max_words: Math.min(64 * 1024 * 1024, Math.max(8 * 1024 * 1024, frames * 8000)),
        timeout_ms: Math.max(120000, frames * 200),
    });
    if (!r.complete) throw new Error(`capture_dl stopped after ${r.frames} of ${frames} frames`);
    if (r.overflow) throw new Error('capture_dl ran out of room — raise max_words');
    return r;
}

const DRAW = 0x3c007878;
const u16 = (b, o) => b[o] | (b[o + 1] << 8);

/**
 * Run attract mode until a round is actually being fought and drawn, then
 * capture it.
 *
 * "Fought and drawn" is three things, each checked rather than assumed: a
 * scene other than the attract front end is loaded, both fighters are playing
 * a motion, and the frame's display list carries model draws. The last one is
 * not redundant — the fighters take their stance and pose for well over a
 * second before the round starts and anything is submitted, and a capture
 * taken there has the rig commands and not a single draw.
 */
export async function captureFight(emu, {
    out = DEFAULT_DL_OUT, frames = 120, maxFrames = 7200, every = 60, log = () => {}, probes = null,
} = {}) {
    fs.mkdirSync(out, { recursive: true });
    const prefix = path.join(out, 'fight');
    const probe = path.join(out, 'probe');
    for (let waited = 0; waited < maxFrames; waited += every) {
        const w = await emu.waitFrames(every, 30000);
        if (!w.reached) throw new Error('the game stopped advancing frames while waiting for a fight');
        const sn = (await emu.readMemory(STAGE_NUM, 1))[0];
        const p1 = await emu.readMemory(P1_ROB + ROB_MOTION, 2);
        const p2 = await emu.readMemory(P2_ROB + ROB_MOTION, 2);
        if (sn === 15 || !u16(p1, 0) || !u16(p2, 0)) continue;
        await captureDl(emu, probe, 2);
        const { words } = loadDl(probe);
        if (!words.includes(DRAW)) continue;
        const scene = await identifyScene(emu);
        log(`a round is on after ${waited + every} frames: stage ${scene.stage}, ` +
            `P1 motion ${u16(p1, 0)}, P2 motion ${u16(p2, 0)}`);
        const r = await captureDl(emu, prefix, frames, { probes });
        fs.writeFileSync(path.join(out, 'fight-scene.json'), JSON.stringify({
            stage: scene.stage, texWords: scene.texWords, words: r.words, frames: r.frames,
            taken: new Date().toISOString(),
        }, null, 2));
        return { prefix, scene, words: r.words, frames: r.frames };
    }
    throw new Error(`no round was fought and drawn within ${maxFrames} frames of attract mode`);
}

/**
 * Capture a fixed window of the game's own frame counter: run until
 * frame_counter reaches `from`, then record `frames` frames. Attract mode is
 * deterministic from boot, so a window names the same stretch of it every run —
 * which is what grading a scripted sequence like the intro needs.
 */
export async function captureWindow(emu, {
    from, frames, out = DEFAULT_DL_OUT, name = 'window', range = null, tgp = false,
    scene = false, slots = false, unit = false, log = () => {},
} = {}) {
    fs.mkdirSync(out, { recursive: true });
    const readCounter = async () => (await emu.readMemory(FRAME_COUNTER, 4)).readUInt32LE(0);
    let now = await readCounter();
    if (now > from) throw new Error(`frame_counter is already ${now}, past ${from} — start a fresh emulator`);
    while (now < from) {
        const step = Math.max(1, Math.min(600, from - now - 1));
        const w = await emu.waitFrames(step, 60000);
        if (!w.reached) throw new Error('the game stopped advancing frames');
        now = await readCounter();
        if (from - now <= 1) break;
    }
    const prefix = path.join(out, name);
    const r = await captureDl(emu, prefix, frames, { range, tgp, scene, slots, unit });
    log(`captured frame_counter ${now}..${now + r.frames}: ${r.words} words -> ${prefix}`);
    return { prefix, words: r.words, frames: r.frames };
}

/* ---- the explorer toolkit's layouts --------------------------------------- */

/**
 * Write the capture again in the layout stf-tools/dl-verify.mjs reads, beside
 * the original: `<prefix>-stage.bin` (a hard link where the file system allows)
 * and `<prefix>-stage.json`, whose marks are
 * [frame, word index, carpet ang_x, heading, roll, sky angle, stage_num, frame_counter].
 */
export function writeStageLayout(prefix, stage) {
    const meta = JSON.parse(fs.readFileSync(`${prefix}.json`, 'utf8'));
    const at = (name) => 2 + PROBES.findIndex(([n]) => n === name);
    const cols = ['carpetAngX', 'carpetHeading', 'carpetRoll', 'skyAngle', 'stageNum', 'frameCounter'].map(at);
    const marks = meta.marks.map((m) => [m[0], m[1], ...cols.map((c) => m[c])]);
    const out = `${prefix}-stage`;
    fs.writeFileSync(`${out}.json`, JSON.stringify({ stage, words: meta.words, marks }));
    fs.rmSync(`${out}.bin`, { force: true });
    try { fs.linkSync(`${prefix}.bin`, `${out}.bin`); } catch { fs.copyFileSync(`${prefix}.bin`, `${out}.bin`); }
    return out;
}
