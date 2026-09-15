/*
 * grade-pose.mjs — hold the coprocessor's rig maths against the explorer's.
 *
 * `tools/README.md` names this as the piece that was missing: the explorer has
 * the rig, the IK chains and the sway chains, and its checks hold them against
 * real captures, but nothing here graded the emulator's COP bone handlers
 * against them. This does the first half — the body matrix (op 0x62) and the
 * four two-bone IK chains (op 0x6B) that place twelve of a fighter's sixteen
 * slots.
 *
 * ## Where the numbers come from
 *
 * Three sides, and the third is what keeps it from being two ports agreeing:
 *
 *   the board      `stf-tools/motion-pose.csv` — every argument the i960 handed
 *                  the coprocessor over 328 frames of a real fight under MAME,
 *                  with the motion and the motion frame beside each one: the
 *                  waist position, the body euler, and per limb the pivot, the
 *                  base euler, the IK target, the two bone lengths and the bend
 *                  direction. Those are the *inputs*, captured off hardware.
 *   the explorer   `js/pose.js` — a second implementation of the same two ops,
 *                  written from the same reverse engineering but separately.
 *   this emulator  `sharc_exec.h`, reached through the `cop_exec` bridge
 *                  command, which pushes raw words at the coprocessor port
 *                  exactly as the i960 does.
 *
 * So the arguments are neither synthetic nor invented by either port: both
 * sides are handed the machine's own numbers and asked what they make of them.
 * The board cannot arbitrate the *answers* — the TGP slots are not readable out
 * of either ADSP's data space, which is why `stf-tools/dl-rig.mjs` has to
 * rebuild them — so a disagreement here says the two ports differ, and which
 * one is wrong is then an argument about kinematics rather than about data.
 *
 * ## What is compared, and what is held equal on purpose
 *
 * An IK chain hangs off the chest (arms) or the pelvis (legs), and neither is
 * in the capture: they are a matrix the board is left holding, built by ops the
 * capture does not spell out as arguments. So this builds both parents from the
 * explorer's sampled motion and pushes them into the emulator explicitly
 * (identity, translate, load-3x3) before each chain. That makes the parent
 * frame identical by construction, so what the comparison measures is the IK op
 * itself and nothing upstream of it.
 *
 * ## The trig rows
 *
 * The explorer takes its cosine from a 256-entry table, quantising an angle to
 * its top byte, because that is what it reads the coprocessor as doing; this
 * emulator calls `cosf` on the full 16 bits. Nothing available here can settle
 * which is the hardware's — `stf-tools/test-head-mame.mjs`, the check that
 * would, says of itself that it is incomplete — so rather than guess, the run
 * is taken twice: once on the board's own angles, and once with every input
 * angle snapped to the table's grid, where the two conventions agree by
 * construction. The gap between the two is the whole cost of the disagreement,
 * and the snapped rows are a clean read on whether the ports differ
 * structurally.
 *
 *   node tools/grade-pose.mjs
 *   node tools/grade-pose.mjs --rows 50        # a quick pass
 *   node tools/grade-pose.mjs --worst 10       # name the worst frames
 *   node tools/grade-pose.mjs --attach
 */
import fs from 'node:fs';
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { loadRom, findRom } from './lib/rom.mjs';
import { nc, noclipVersion, REPO } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['rows', 'port', 'worst', 'csv']);
const rep = new Report('grade-pose — body matrix and IK chains, emulator vs explorer');

/* ---- the board's capture -------------------------------------------------- */

/* The capture lives in the explorer's toolkit rather than here, for the same
 * reason no ROM does: it is the game's own data, read off a running machine.
 * Resolution order matches lib/noclip.mjs — an override first, then the places
 * a checkout of this repo is likely to sit beside one. */
function findCsv() {
    const named = args.str('csv');
    if (named) return fs.existsSync(named) ? named : null;
    const tried = [];
    for (const base of [process.env.M2_STF_TOOLS,
                        path.resolve(REPO, '..', 'stf-tools'),
                        path.resolve(REPO, '..', '..', 'stf-tools')]) {
        if (!base) continue;
        const p = path.join(base, 'motion-pose.csv');
        tried.push(p);
        if (fs.existsSync(p)) return p;
    }
    rep.note('looked for motion-pose.csv in:');
    for (const t of tried) rep.note('  ' + t);
    return null;
}

/*
 * One row per captured frame. `since` is how many frames the motion has been
 * playing: `play_motion` eases out of the previous pose over the first eight
 * through `smooth_int`, so those frames are not the motion the explorer would
 * play, and its own check holds them back rather than asserting on them. This
 * drops them for the same reason — but only from the row that checks the inputs
 * against the ROM. The op comparison is unaffected either way, since both sides
 * are handed the board's numbers whatever they are.
 */
const BLEND = 8;

function readCsv(file) {
    const lines = fs.readFileSync(file, 'utf8').trim().split(/\r?\n/);
    const rows = [];
    for (const line of lines.slice(1)) {
        const v = line.split(',').map(Number);
        let i = 0;
        const row = {
            motion: v[i++], coma: v[i++], char: v[i++], since: v[i++],
            pos: [v[i++], v[i++], v[i++]], euler: [v[i++], v[i++], v[i++]], limbs: [],
        };
        for (let k = 0; k < 4; k++) {
            row.limbs.push({
                pivot: [v[i++], v[i++], v[i++]], euler: [v[i++], v[i++], v[i++]],
                target: [v[i++], v[i++], v[i++]],
                lower: v[i++], upper: v[i++], flip: v[i++],
            });
        }
        rows.push(row);
    }
    return rows;
}

/* The four chains in the order `calc_rob_angle_cont` emits them, with the
 * motion objects each takes its base euler and target from and the two TGP
 * slots it writes. Argument 14 names the lower bone — forearm or shin — and
 * argument 15 the upper: TGP addresses step 0x0C a slot from 0x3A00 for P1, and
 * the pairs come straight off the slot numbering in js/pose.js. */
const TGP_P1 = 0x3a00, TGP_STRIDE = 0x0c;
const CHAINS = [
    { name: 'L arm', upperSlot: 3, lowerSlot: 4, baseAngle: 7, target: 3 },
    { name: 'R arm', upperSlot: 6, lowerSlot: 7, baseAngle: 8, target: 4 },
    { name: 'L leg', upperSlot: 10, lowerSlot: 11, baseAngle: 10, target: 6 },
    { name: 'R leg', upperSlot: 13, lowerSlot: 14, baseAngle: 11, target: 7 },
];

/* ---- the two tolerances, and what each one means -------------------------- */

/*
 * STRUCT — what is left once the trig conventions are held equal.
 *
 * The two ports do the same arithmetic in different widths: this emulator in
 * float32, as the SHARC does, and the explorer in float64, as JavaScript does.
 * That is invisible almost everywhere and not at all invisible where the law of
 * cosines ends: `sqrt(1 - c*c)` with c approaching 1 throws away the leading
 * digits, so the error there goes as the square root of the epsilon rather than
 * the epsilon — about 3e-4 at worst for float32.
 *
 * Measured, the residual is 1.2e-5 over 2624 limb transforms and every one of
 * the worst is a limb at reach 1.000, stretched dead straight at a target it
 * can only just span, which is exactly where that cancellation bites. `--worst`
 * prints the reach beside each disagreement so this stays checkable rather than
 * asserted. So: a hundredth of a degree, an order below the floor and two
 * clear of anything a wrong rule could hide in.
 */
const STRUCT = 1e-4;

/*
 * TABLE — what the disagreement about cosine can account for.
 *
 * The explorer quantises an angle to its top byte and looks the cosine up in a
 * 256-entry table; this emulator calls cosf on all sixteen bits. That is worth
 * at most sin(pi/256) = 1.2e-2 per factor, and a matrix is a few factors, so
 * anything up to about 5e-2 is the conventions differing and nothing else.
 *
 * These rows are not slack versions of the strict ones. They are the check that
 * the trig convention is the *only* thing that differs on the board's own
 * angles: something structural would push them past a bound the table cannot
 * reach, and the strict rows above would have caught it anyway.
 */
const TABLE = 5e-2;

/* ---- word building -------------------------------------------------------- */

const fb = new DataView(new ArrayBuffer(4));
const bits = (f) => { fb.setFloat32(0, f, true); return fb.getUint32(0, true); };
const w = (v) => (v >>> 0).toString(16).toUpperCase().padStart(8, '0');
const words = (...vs) => vs.map(w).join('');

/* The command word for coprocessor op n: (n<<23) | (n<<8) | n. */
const OP = (n) => (((n << 23) | (n << 8) | n) >>> 0);
const OP_IDENT = OP(0x03), OP_TRANS = OP(0x06), OP_LOAD33 = OP(0x11);
const OP_BODY = OP(0x62), OP_IK = OP(0x6b);

/* The waist, as the board's own set_body arguments: position, the body euler
 * sent (z, y, x), and the fighter's facing — which the capture does not carry,
 * so both sides go without it, as the explorer does by default. */
function bodyWords(row) {
    return words(OP_IDENT, OP_BODY,
        bits(row.pos[0]), bits(row.pos[1]), bits(row.pos[2]),
        row.euler[0], row.euler[1], row.euler[2], 0, 0, 0);
}

/* One chain: plant the parent frame the arms and legs actually hang off, then
 * the chain itself. The identity zeroes the translation too, so the translate
 * that follows lands the parent's position exactly rather than accumulating. */
function chainWords(row, k, parent) {
    const c = CHAINS[k], L = row.limbs[k];
    return words(OP_IDENT,
        OP_TRANS, bits(parent.t[0]), bits(parent.t[1]), bits(parent.t[2]),
        OP_LOAD33, ...parent.r.map(bits),
        OP_IK,
        bits(L.pivot[0]), bits(L.pivot[1]), bits(L.pivot[2]),
        L.euler[0], L.euler[1], L.euler[2],
        0, 0, 0,
        bits(L.target[0]), bits(L.target[1]), bits(L.target[2]),
        bits(L.lower), bits(L.upper),
        TGP_P1 + c.lowerSlot * TGP_STRIDE,
        TGP_P1 + c.upperSlot * TGP_STRIDE,
        L.flip);
}

/* ---- the explorer's frame ------------------------------------------------- */

/*
 * `buildPose` ends by turning every slot a quarter turn about Y — where the
 * fighter faces once the parts are drawn — but the coprocessor's TGP slots hold
 * the matrix before that, which is what `dump_tgp` reads back. So undo it: the
 * turn takes a column (x, y, z) to (z, y, -x), and this takes it back. Index
 * shuffling and sign flips, so nothing is lost doing it.
 */
function unturn({ r, t }) {
    const out = new Array(9);
    for (let c = 0; c < 3; c++) {
        out[c * 3] = -r[c * 3 + 2];
        out[c * 3 + 1] = r[c * 3 + 1];
        out[c * 3 + 2] = r[c * 3];
    }
    return { r: out, t: [-t[2], t[1], t[0]] };
}

/* The board's numbers, dropped over the explorer's sample. Everything the
 * capture recorded comes from the capture; the chest and head channels it did
 * not record stay as the ROM samples them, and drive both sides equally. */
function boardSample(sample, row) {
    const angles = Uint16Array.from(sample.angles);
    const targets = Float32Array.from(sample.targets);
    for (let i = 0; i < 3; i++) targets[i] = row.pos[i];
    angles[4 * 3 + 2] = row.euler[0];
    angles[4 * 3 + 1] = row.euler[1];
    angles[4 * 3 + 0] = row.euler[2];
    for (let k = 0; k < 4; k++) {
        const c = CHAINS[k], L = row.limbs[k];
        angles[c.baseAngle * 3 + 2] = L.euler[0];
        angles[c.baseAngle * 3 + 1] = L.euler[1];
        angles[c.baseAngle * 3 + 0] = L.euler[2];
        for (let i = 0; i < 3; i++) targets[c.target * 3 + i] = L.target[i];
    }
    return { ...sample, angles, targets };
}

const boardSkeleton = (skel, row) => ({
    ...skel,
    pivot: row.limbs.map((L) => L.pivot),
    upper: row.limbs.map((L) => L.upper),
    lower: row.limbs.map((L) => L.lower),
});

/* Snap every angle the run feeds in to a multiple of 0x100 — the grid the
 * explorer's 256-entry cosine table lands on — so the two trig conventions
 * agree and anything left over is a structural difference. */
function snapAngles(row) {
    const snap = (a) => a & 0xff00;
    return {
        ...row,
        euler: row.euler.map(snap),
        limbs: row.limbs.map((L) => ({ ...L, euler: L.euler.map(snap) })),
    };
}

/* ---- run ------------------------------------------------------------------ */

const csvPath = findCsv();
if (!csvPath) {
    rep.skip('the board capture is present',
             'no motion-pose.csv — clone stf-tools beside this repo, or pass --csv');
    rep.finish();
    process.exit();
}

const allRows = readCsv(csvPath);
const limit = args.num('rows', 0);
const rows = limit > 0 ? allRows.slice(0, limit) : allRows;
rep.note(`${rows.length} frames off the board (${csvPath})`);
rep.note(`motions ${[...new Set(rows.map((r) => r.motion))].join(', ')}`
       + ` · characters ${[...new Set(rows.map((r) => r.char))].join(', ')}`);

const { readCharacter } = await nc('characters.js');
const { decodeMotion, sampleMotion } = await nc('motion.js');
const { buildPose } = await nc('pose.js');
rep.note(`explorer at ${noclipVersion()}`);

const { rom } = await loadRom();

const port = args.num('port', 7172);
const emu = args.bool('attach')
    ? await M2Hle.attach({ port })
    : await M2Hle.launch({ rom: findRom().primary, port, run: false });

const charCache = new Map(), motionCache = new Map();
const getChar = (i) => {
    if (!charCache.has(i)) charCache.set(i, readCharacter(rom, i));
    return charCache.get(i);
};
const getMotion = (i) => {
    if (!motionCache.has(i)) motionCache.set(i, decodeMotion(rom, i));
    return motionCache.get(i);
};

const maxAbs = (a, b) => a.reduce((m, v, i) => Math.max(m, Math.abs(v - b[i])), 0);

/**
 * One pass over the frames.
 *
 * @param {boolean} snapped  feed both sides angles on the cosine table's grid
 */
async function pass(snapped) {
    const acc = { frames: 0, slots: 0, missing: 0,
                  bodyR: 0, bodyT: 0, limbR: 0, limbT: 0, worst: [] };
    for (const rawRow of rows) {
        const row = snapped ? snapAngles(rawRow) : rawRow;
        const ch = getChar(row.char), mot = getMotion(row.motion);
        if (!ch || !mot) continue;

        const sample = boardSample(sampleMotion(rom, mot, row.coma), row);
        const skel = boardSkeleton(ch.skeleton, row);
        const pose = buildPose(skel, sample).map(unturn);

        /* The body matrix, straight out of op 0x62: the coprocessor is left
         * holding it, so it is the current matrix rather than a slot. */
        await emu.rpc('cop_exec', { words: bodyWords(row), reset: 1 });
        const body = await emu.rpc('dump_tgp');
        acc.bodyR = Math.max(acc.bodyR, maxAbs(body.rot, pose[0].r));
        acc.bodyT = Math.max(acc.bodyT, maxAbs(body.pos, pose[0].t));

        /* The four chains, each on the parent it actually hangs off. */
        let stream = '';
        for (let k = 0; k < 4; k++) stream += chainWords(row, k, pose[k < 2 ? 1 : 9]);
        await emu.rpc('cop_exec', { words: stream, reset: 1 });
        const tgp = (await emu.rpc('dump_tgp')).tgp;

        acc.frames++;
        for (let k = 0; k < 4; k++) {
            const c = CHAINS[k];
            for (const slot of [c.upperSlot, c.lowerSlot]) {
                const got = tgp[slot], want = pose[slot];
                acc.slots++;
                if (got.every((v) => v === 0)) { acc.missing++; continue; }
                const dR = maxAbs(got.slice(0, 9), want.r);
                const dT = maxAbs(got.slice(9, 12), want.t);
                acc.limbR = Math.max(acc.limbR, dR);
                acc.limbT = Math.max(acc.limbT, dT);
                /* How close to straight this limb is. The law of cosines ends in
                 * sqrt(1 - c*c), which loses its leading digits as c goes to 1,
                 * so a chain reaching for a target it can only just span is
                 * where float32 and float64 part company first. Carrying it
                 * beside the disagreement is what separates a precision floor
                 * from a bug: a residual that only appears at reach ~ 1 is the
                 * former. The pivot is the upper bone's own position, which is
                 * where the chain hangs. */
                const P = pose[c.upperSlot].t, L = row.limbs[k];
                const reach = Math.hypot(L.target[0] - P[0], L.target[1] - P[1],
                                         L.target[2] - P[2]) / (L.lower + L.upper);
                acc.worst.push({ motion: row.motion, coma: row.coma, char: row.char,
                                 name: c.name, slot, dR, dT, reach });
            }
        }
    }
    acc.worst.sort((a, b) => Math.max(b.dR, b.dT) - Math.max(a.dR, a.dT));
    return acc;
}

try {
    await emu.waitForRom();

    /* ---- 1. the reference side is still anchored to the machine ---------- */

    /* Before anything is graded against the explorer, check the explorer still
     * decodes the arguments the board sent. If this fails, the run is measuring
     * against a reference that has drifted and every row below it is noise. */
    let inputRows = 0, worstAng = 0, worstPos = 0;
    const angDiff = (a, b) => {
        const d = Math.abs((a - b) & 0xffff);
        return Math.min(d, 0x10000 - d);
    };
    for (const row of rows) {
        if (row.since < BLEND) continue;
        const mot = getMotion(row.motion);
        if (!mot) continue;
        const s = sampleMotion(rom, mot, row.coma);
        inputRows++;
        for (let i = 0; i < 3; i++)
            worstPos = Math.max(worstPos, Math.abs(row.pos[i] - s.targets[i]));
        for (const [k, axis] of [[0, 2], [1, 1], [2, 0]])
            worstAng = Math.max(worstAng, angDiff(row.euler[k], s.angles[12 + axis]));
        for (let k = 0; k < 4; k++) {
            const c = CHAINS[k], L = row.limbs[k];
            for (const [k2, axis] of [[0, 2], [1, 1], [2, 0]])
                worstAng = Math.max(worstAng, angDiff(L.euler[k2], s.angles[c.baseAngle * 3 + axis]));
            for (let i = 0; i < 3; i++)
                worstPos = Math.max(worstPos, Math.abs(L.target[i] - s.targets[c.target * 3 + i]));
        }
    }
    rep.check('the explorer still samples what the board sent',
              inputRows > 0 && worstAng <= 1 && worstPos < 1e-3,
              `${inputRows} frames past the ${BLEND}-frame blend; angles within `
            + `${worstAng} binary radians, targets within ${worstPos.toExponential(2)}`);

    /* ---- 2. the ops, on the board's own angles --------------------------- */

    const raw = await pass(false);
    rep.note(`${raw.frames} frames replayed through the coprocessor port`
           + ` (${raw.slots} limb transforms)`);
    rep.check('op 0x62 — the body matrix, nothing but the cosine table apart', raw.bodyR < TABLE && raw.bodyT < TABLE,
              `rotation within ${raw.bodyR.toExponential(2)}, position within `
            + `${raw.bodyT.toExponential(2)}`);
    rep.check('op 0x6B — every limb slot is written', raw.missing === 0,
              raw.missing ? `${raw.missing} of ${raw.slots} slots came back all zero`
                          : `${raw.slots} slots written`);
    rep.check('op 0x6B — limb rotations, nothing but the cosine table apart', raw.limbR < TABLE,
              `within ${raw.limbR.toExponential(2)}`);
    rep.check('op 0x6B — limb positions, nothing but the cosine table apart', raw.limbT < TABLE,
              `within ${raw.limbT.toExponential(2)} world units`);

    /* ---- 3. the same, with the trig conventions held equal ---------------- */

    const snapped = await pass(true);
    rep.note('the same frames with every input angle on the cosine table\'s grid,'
           + ' where the two trig conventions agree');
    rep.check('op 0x62 — the body matrix, trig held equal',
              snapped.bodyR < STRUCT && snapped.bodyT < STRUCT,
              `rotation within ${snapped.bodyR.toExponential(2)}, position within `
            + `${snapped.bodyT.toExponential(2)}`);
    rep.check('op 0x6B — limb rotations, trig held equal', snapped.limbR < STRUCT,
              `within ${snapped.limbR.toExponential(2)}`);
    rep.check('op 0x6B — limb positions, trig held equal', snapped.limbT < STRUCT,
              `within ${snapped.limbT.toExponential(2)} world units`);
    rep.note(`the cosine table accounts for ${(raw.limbR - snapped.limbR).toExponential(2)}`
           + ` of rotation and ${(raw.limbT - snapped.limbT).toExponential(2)} of position`);

    const nWorst = args.num('worst', 0);
    if (nWorst > 0) {
        const show = (label, list) => {
            rep.note(label);
            for (const x of list.slice(0, nWorst)) {
                rep.note(`  motion ${x.motion} frame ${x.coma} char ${x.char} ${x.name}`
                       + ` slot ${x.slot} reach ${x.reach.toFixed(3)}:`
                       + ` rotation ${x.dR.toExponential(2)},`
                       + ` position ${x.dT.toExponential(2)}`);
            }
        };
        show('worst on the board\'s own angles (the cosine table dominates here):', raw.worst);
        show('worst with the trig held equal (structure, and the float32 floor):',
             snapped.worst);
    }
} finally {
    await emu.close({ kill: !args.bool('attach') });
}

rep.finish();
