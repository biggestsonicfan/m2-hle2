/*
 * grade-motion.mjs — both fighters' animation, as this emulator's i960 plays it,
 * against the explorer's motion decoder.
 *
 * grade-pose.mjs grades the coprocessor: it hands the emulator the arguments a
 * real board sent and checks what comes back. This grades the half in front of
 * that. Every frame the i960 samples each fighter's motion and sends the result
 * to the coprocessor as one set_body (op 0x62) and four ik_2bone (op 0x6B) —
 * the waist position, the body and limb eulers, the IK targets, the pivots, the
 * bone lengths and the bend direction. Those are what `js/motion.js` samples and
 * `js/pose.js` solves with, so the two can be held against each other number
 * for number, at the motion and the frame the game says it was on.
 *
 * It is stf-tools/test-motion-mame.mjs pointed at this emulator instead of at
 * MAME, with the same tolerances, and widened to both players. The capture
 * reads each fighter's motion number, motion frame and character straight out
 * of its work structure at every frame edge (lib/board.mjs), and an ik_2bone
 * says whose limb it is by the TGP slot window it writes into, 0x3A00 for P1
 * and 0x3B00 for P2 — so nothing depends on the order the two are emitted in.
 *
 * ## The frames a motion is blended on
 *
 * play_motion does not cut to a new motion: smooth_int eases in from the pose
 * the fighter was already in, and eases out toward the next motion near the
 * end. The explorer plays a motion outright, so those frames are the one place
 * the two are expected to differ; they are reported with how far apart they
 * are, and not asserted on.
 *
 * The MAME check holds back the first eight frames of a motion, which is what
 * the two motions it was measured on do. Across a whole round that is not
 * enough: the ease-in is four or eight frames by the motion's length, counted
 * on the motion frame or on a separate counter, and the ease-out is at the
 * other end entirely. So the capture reads the blend state itself at every
 * frame edge (lib/board.mjs, ROB_SMOOTH_*) and a frame is held back exactly
 * when get_frame_dat blended it (lib/dl.mjs, blendOf).
 *
 *   node tools/grade-motion.mjs                    # capture a fight, then grade it
 *   node tools/grade-motion.mjs --capture <prefix> # grade an existing capture
 *   node tools/grade-motion.mjs --frames 300 --worst 20
 *   node tools/grade-motion.mjs --from 240 --frames 2200   # the attract intro
 */
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { loadRom, findRom } from './lib/rom.mjs';
import { nc, noclipVersion } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { TGP_WINDOW } from './lib/board.mjs';
import {
    captureFight, captureWindow, loadDl, segment, f32, blendOf, sampledFrame, DEFAULT_DL_OUT, COPRO_FIFO,
    mirrorSample, mirroredOf,
} from './lib/dl.mjs';

const args = parseArgs(['capture', 'frames', 'port', 'out', 'worst', 'from']);
const rep = new Report('grade-motion — both fighters\' motion, emulator i960 vs explorer');

/* The four chains in the order calc_rob_angle_cont emits them: the motion
 * objects each takes its base euler and IK target from, the lower-bone slot
 * that names it (the low byte, inside either fighter's window), and the bend
 * direction it is sent with. From stf-tools/test-motion-mame.mjs. */
const CHAINS = [
    { name: 'L arm', slot: 0x30, baseAngle: 7, target: 3, flip: 0 },
    { name: 'R arm', slot: 0x54, baseAngle: 8, target: 4, flip: 0 },
    { name: 'L leg', slot: 0x84, baseAngle: 10, target: 6, flip: 1 },
    { name: 'R leg', slot: 0xa8, baseAngle: 11, target: 7, flip: 1 },
];
const CHAIN_OF = new Map(CHAINS.map((c, i) => [c.slot, i]));

/* Tolerances, the MAME check's: a float the board computes in 32 bits, an
 * angle to a binary radian. */
const TOL = { pos: 1e-3, ang: 1, tgt: 1e-3, pivot: 1e-4, bone: 1e-5 };
const angDiff = (a, b) => { const d = Math.abs((a - b) & 0xffff); return Math.min(d, 0x10000 - d); };

/* ---- the capture ----------------------------------------------------------- */

let prefix = args.str('capture');
if (!prefix) {
    const port = args.num('port', 7172);
    const emu = await M2Hle.launch({ rom: findRom().primary, port });
    try {
        await emu.waitUntilRunning();
        const out = args.str('out', DEFAULT_DL_OUT);
        const cap = args.has('from')
            ? await captureWindow(emu, { from: args.num('from'), frames: args.num('frames', 240),
                                         out, name: 'motion', range: COPRO_FIFO, log: (s) => rep.note(s) })
            : await captureFight(emu, { out, frames: args.num('frames', 240), log: (s) => rep.note(s) });
        prefix = cap.prefix;
        rep.note(`captured ${cap.frames} frames, ${cap.words} words -> ${prefix}`);
    } finally {
        await emu.close();
    }
}
const dl = loadDl(prefix);

/* ---- rows: what each fighter was posed with, per frame -------------------- */

const rows = [[], []];
for (const fr of dl.frames) {
    const blocks = [{ body: null, iks: new Map() }, { body: null, iks: new Map() }];
    let body = null;
    for (const c of segment(fr.copro)) {
        if (c.op === 0x62 && c.args.length >= 9) body = c;
        else if (c.op === 0x6b && c.args.length >= 17) {
            const slot = c.args[14] & 0xffff;
            const p = TGP_WINDOW.indexOf(slot & 0xff00);
            const k = CHAIN_OF.get(slot & 0xff);
            if (p < 0 || k === undefined) continue;
            if (!blocks[p].body) blocks[p].body = body;
            blocks[p].iks.set(k, c);
        }
    }
    for (let p = 0; p < 2; p++) {
        const key = `p${p + 1}`;
        const motion = fr.closed[key + 'Motion'], coma = fr.closed[key + 'Coma'];
        if (!motion) continue;
        /* A slice in which the motion restarted straddles the reset; which side
         * its commands fell on is not something the capture can say. */
        if (motion !== fr.opened[key + 'Motion'] || coma < fr.opened[key + 'Coma']) continue;
        const b = blocks[p];
        if (!b.body || b.iks.size !== 4) continue;
        const row = { frame: fr.frame, motion, coma, probes: fr.closed, key, char: fr.closed[key + 'Char'],
                      skeleton: fr.closed[key + 'Skeleton'], blend: blendOf(fr.closed, key),
                      pos: [], euler: [], limbs: [] };
        for (let k = 0; k < 3; k++) row.pos.push(f32(b.body.args[k]));
        for (const k of [3, 4, 5]) row.euler.push(b.body.args[k] & 0xffff);
        for (let k = 0; k < 4; k++) {
            const a = b.iks.get(k).args;
            row.limbs.push({
                pivot: [0, 1, 2].map((i) => f32(a[i])),
                euler: [3, 4, 5].map((i) => a[i] & 0xffff),
                target: [9, 10, 11].map((i) => f32(a[i])),
                lower: f32(a[12]), upper: f32(a[13]), flip: a[16] & 0xff,
            });
        }
        rows[p].push(row);
    }
}

/* ---- the explorer's side, and the comparison ------------------------------ */

const { rom } = await loadRom();
const { readCharacter } = await nc('characters.js');
const { readSkeleton } = await nc('pose.js');
const { decodeMotion, sampleMotion } = await nc('motion.js');
const { xtraToMainData, XTRA_DATA_BASE } = await nc('romset.js');

/* A byte of ROM at an i960 address: the program ROM, main data at 0x02000000,
 * or the XTRA_DATA window that mirrors it. The time-warp tables live in one of
 * those; anything else is RAM, which a ROM read cannot answer. */
let unreadable = 0;
function readByte(addr) {
    if (addr < rom.maincpu.length) return rom.maincpu[addr];
    if (addr >= 0x02000000 && addr - 0x02000000 < rom.mainData.length) return rom.mainData[addr - 0x02000000];
    if (addr >= XTRA_DATA_BASE) return rom.mainData[xtraToMainData(addr)];
    unreadable++;
    return 0;
}
rep.note(`explorer ${noclipVersion()}, capture ${path.basename(prefix)}: ${dl.frames.length} frames`);

const motionCache = new Map();
const getMotion = (i) => { if (!motionCache.has(i)) motionCache.set(i, decodeMotion(rom, i)); return motionCache.get(i); };

/*
 * Which skeleton a fighter is posed with.
 *
 * Not the one its character record names. calc_rob_angle_int (0x2EF38) copies
 * SKELETON_TYPE_DATA[skeleton type][character] into the fighter's structure,
 * and calc_rob_angle_cont poses with that copy — so in attract, where the type
 * is 0 and the type-0 table hands most of the cast Sonic's skeleton, Tails and
 * Bean really are posed on Sonic's bones (stf-tools/mame-motion-capture.lua
 * found the same off the board). The explorer's characters.js knows the table
 * and reads it for a character's own type; this reads it for the type the game
 * is actually in, which the capture records every frame.
 *
 * And it copies it only when the skeleton type changes: the routine compares
 * the type at +0x84C with the last one resolved, which set_motion keeps at
 * +0x85B, and skips the copy when they agree. So a character swapped in at the
 * same type keeps the bones that were there — the attract intro changes P2 from
 * Bean to Tails without a type change, and Tails is posed on the skeleton the
 * rig already held. The grader follows that: a fighter's rig is re-resolved to
 * SKELETON_TYPE_DATA[type][character] when its type changes, and taken from
 * whichever table entry the first frame matches before that.
 *
 * Which character's skeleton the rig holds is noted, since "posed on someone
 * else's bones" is worth seeing, but it is the game's rule and not a
 * disagreement. A frame whose rig matches nothing the rule predicts is.
 */
const SKELETON_TYPE_DATA = 0x000c2068;
const skeletonCache = new Map();
function expectedSkeleton(type, char) {
    const key = `${type}/${char}`;
    if (skeletonCache.has(key)) return skeletonCache.get(key);
    const dv = rom.mainCpuView;
    const table = type < 4 ? dv.getUint32(SKELETON_TYPE_DATA + type * 4, true) : 0;
    const ptr = table && table + char * 4 + 4 <= rom.maincpu.length ? dv.getUint32(table + char * 4, true) : 0;
    const v = ptr && ptr + 16 * 12 <= rom.maincpu.length ? { ptr, skel: readSkeleton(rom, ptr) } : null;
    skeletonCache.set(key, v);
    return v;
}
/* Whose skeleton a table entry is, for the note: the character whose type-1
 * (fighting) entry is that pointer. */
const ownerOf = new Map();
const tableEntries = [];
for (const t of [1, 0, 2, 3]) {
    for (let i = 0; i < 43; i++) {
        const e = expectedSkeleton(t, i);
        const c = e && readCharacter(rom, i);
        if (!e) continue;
        tableEntries.push(e);
        if (c && !ownerOf.has(e.ptr)) ownerOf.set(e.ptr, c.name);
    }
}
const rigFits = (r, skel) => r.limbs.every((L, k) =>
    L.pivot.every((v, i) => Math.abs(v - skel.pivot[k][i]) <= TOL.pivot)
    && Math.abs(L.lower - skel.lower[k]) <= TOL.bone && Math.abs(L.upper - skel.upper[k]) <= TOL.bone);

for (let p = 0; p < 2; p++) {
    const who = `P${p + 1}`;
    const rs = rows[p];
    if (!rs.length) { rep.skip(`${who} posed`, 'no frame of this fighter could be compared'); continue; }
    const motions = [...new Set(rs.map((r) => r.motion))];
    const chars = [...new Set(rs.map((r) => r.char))];
    rep.note(`${who}: character ${chars.join('/')}, ${rs.length} frames over motions ${motions.join(', ')}`);

    const bad = { pos: [], ang: [], tgt: [], pivot: [], bone: [], flip: [] };
    const rigs = new Map();
    let rig = null, rigType = null;
    const worst = { pos: 0, ang: 0, tgt: 0 };
    const blend = { in: [], out: [] };
    let compared = 0, undecodable = 0, warped = 0, mirrorFrames = 0;
    for (const r of rs) {
        const m = getMotion(r.motion);
        if (!m) { undecodable++; continue; }
        let how;
        if (rig === null) {
            rig = tableEntries.find((e) => rigFits(r, e.skel)) ?? expectedSkeleton(r.skeleton, r.char);
            how = 'resolved before the capture';
        } else if (r.skeleton !== rigType) {
            rig = expectedSkeleton(r.skeleton, r.char) ?? rig;
            how = `re-resolved at a type change, character ${r.char} type ${r.skeleton}`;
        } else {
            how = rigs.size ? null : 'held';
        }
        rigType = r.skeleton;
        if (!rig) { undecodable++; continue; }
        /* Several characters can share one skeleton — Bean's is Fang's — so name
         * it after the character the game names when that character owns it. */
        const namedOwns = [0, 1, 2, 3].some((t) => expectedSkeleton(t, r.char)?.ptr === rig.ptr);
        const owner = namedOwns ? readCharacter(rom, r.char)?.name : ownerOf.get(rig.ptr);
        const rigName = `${owner ?? '0x' + rig.ptr.toString(16)}'s skeleton`;
        const t = rigs.get(rigName) ?? { n: 0, how, chars: new Set() };
        t.n++; t.chars.add(r.char);
        rigs.set(rigName, t);
        const c = { skeleton: rig.skel };
        const f = sampledFrame(r.probes, r.key, readByte);
        if (f !== r.coma) warped++;
        const mirrored = mirroredOf(r.probes, r.key);
        if (mirrored) mirrorFrames++;
        const s = mirrored ? mirrorSample(sampleMotion(rom, m, f)) : sampleMotion(rom, m, f);
        const at = `motion ${r.motion} frame ${r.coma}${f !== r.coma ? ` (sampled at ${f.toFixed(3)})` : ''}`;

        if (r.blend) {
            let e = 0;
            for (let i = 0; i < 3; i++) e = Math.max(e, Math.abs(r.pos[i] - s.targets[i]));
            for (let k = 0; k < 4; k++) {
                for (let i = 0; i < 3; i++) {
                    e = Math.max(e, Math.abs(r.limbs[k].target[i] - s.targets[CHAINS[k].target * 3 + i]));
                }
            }
            blend[r.blend].push(e);
            continue;
        }
        compared++;

        for (let i = 0; i < 3; i++) {
            const d = Math.abs(r.pos[i] - s.targets[i]);
            worst.pos = Math.max(worst.pos, d);
            if (d > TOL.pos) bad.pos.push({ at, d, what: `waist[${i}] ${r.pos[i]} vs ${s.targets[i]}` });
        }
        for (const [k, axis] of [[0, 2], [1, 1], [2, 0]]) {
            const d = angDiff(r.euler[k], s.angles[4 * 3 + axis]);
            worst.ang = Math.max(worst.ang, d);
            if (d > TOL.ang) bad.ang.push({ at, d, what: `body axis ${axis} ${r.euler[k]} vs ${s.angles[12 + axis]}` });
        }
        for (let k = 0; k < 4; k++) {
            const L = r.limbs[k], ch = CHAINS[k];
            for (let i = 0; i < 3; i++) {
                const d = Math.abs(L.pivot[i] - c.skeleton.pivot[k][i]);
                if (d > TOL.pivot) {
                    bad.pivot.push({ at, d, what: `${ch.name} pivot[${i}] ${L.pivot[i]} vs ${c.skeleton.pivot[k][i]} (skeleton type ${r.skeleton})` });
                }
            }
            for (const [k2, axis] of [[0, 2], [1, 1], [2, 0]]) {
                const want = s.angles[ch.baseAngle * 3 + axis];
                const d = angDiff(L.euler[k2], want);
                worst.ang = Math.max(worst.ang, d);
                if (d > TOL.ang) bad.ang.push({ at, d, what: `${ch.name} axis ${axis} ${L.euler[k2]} vs ${want}` });
            }
            for (let i = 0; i < 3; i++) {
                const want = s.targets[ch.target * 3 + i];
                const d = Math.abs(L.target[i] - want);
                worst.tgt = Math.max(worst.tgt, d);
                if (d > TOL.tgt) bad.tgt.push({ at, d, what: `${ch.name} target[${i}] ${L.target[i]} vs ${want}` });
            }
            if (Math.abs(L.lower - c.skeleton.lower[k]) > TOL.bone || Math.abs(L.upper - c.skeleton.upper[k]) > TOL.bone) {
                bad.bone.push({ at, d: 0, what: `${ch.name} ${L.lower}/${L.upper} vs ${c.skeleton.lower[k]}/${c.skeleton.upper[k]}` });
            }
            if (L.flip !== ch.flip) bad.flip.push({ at, d: 0, what: `${ch.name} flip ${L.flip} vs ${ch.flip}` });
        }
    }

    if (undecodable) rep.note(`${who}: ${undecodable} frames name a motion or character the explorer cannot decode`);
    if (warped) rep.note(`${who}: ${warped} frames sampled off the motion frame (time-warp table or replay)`);
    if (mirrorFrames) rep.note(`${who}: ${mirrorFrames} frames played mirrored (set_mirror)`);
    if (unreadable) rep.note(`${who}: ${unreadable} time-warp bytes pointed outside the ROM and read as zero`);
    if (!compared) { rep.skip(`${who} motion`, 'every comparable frame was inside a blend'); continue; }
    const frames = (list) => new Set(list.map((x) => x.at)).size;
    const line = (name, list, detail) => rep.check(`${who} ${name}`, !list.length,
        `${detail}; ${list.length ? `${frames(list)} of ${compared} frames out` : `${compared} frames`}`);
    line('waist position', bad.pos, `worst ${worst.pos.toExponential(2)}`);
    line('joint angles', bad.ang, `worst ${worst.ang} brad`);
    line('IK targets', bad.tgt, `worst ${worst.tgt.toExponential(2)}`);
    line('limb pivots', bad.pivot, 'against the rig SKELETON_TYPE_DATA resolved');
    line('bone lengths', bad.bone, 'against the rig SKELETON_TYPE_DATA resolved');
    for (const [name, t] of rigs) {
        rep.note(`  ${who} ${t.n} frames posed on ${name} while the game named character ` +
                 `${[...t.chars].join('/')}${t.how ? ` (${t.how})` : ''}`);
    }
    line('bend direction', bad.flip, 'per chain');

    const all = Object.values(bad).flat().sort((a, b) => b.d - a.d);
    /* By motion and channel first: a wrong curve shows up as one channel of
     * one motion over a run of frames, and that is the unit worth chasing. */
    const groups = new Map();
    for (const x of all) {
        const motion = x.at.match(/^motion (\d+)/)[1];
        const channel = x.what.replace(/\s-?[\d.e+-]+ vs -?[\d.e+-]+.*$/, '').replace(/\s\(skeleton.*$/, '');
        const k = `motion ${motion}: ${channel}`;
        const g = groups.get(k) ?? { n: 0, worst: x, frames: new Set() };
        g.n++; g.frames.add(x.at);
        if (x.d > g.worst.d) g.worst = x;
        groups.set(k, g);
    }
    for (const [k, g] of [...groups].sort((a, b) => b[1].frames.size - a[1].frames.size)) {
        rep.note(`  ${who} ${k} — ${g.frames.size} frames, worst ${g.worst.at}: ${g.worst.what}`);
    }
    for (const x of all.slice(0, args.num('worst', 0))) rep.note(`  ${who} ${x.at}: ${x.what}`);
    for (const kind of ['in', 'out']) {
        const v = blend[kind].sort((a, b) => a - b);
        if (v.length) {
            rep.note(`  ${who} easing ${kind}: ${v.length} frames held back, worst ${v[v.length - 1].toExponential(2)}` +
                     ` median ${v[v.length >> 1].toExponential(2)} (reported only)`);
        }
    }
}

rep.finish();
