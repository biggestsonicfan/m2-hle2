/*
 * grade-stages.mjs — every arena, with its animations and its draw routines, as
 * this emulator runs them, against the explorer's stage builder.
 *
 * One round is captured per stage: the stage is picked where ROUND_INIT hands it
 * to change_scene (lib/dl.mjs captureStage), and the capture carries every live
 * stage object's age and a bufferram snapshot at each mark. Four things are
 * measured off it.
 *
 * 1. Placement. Every arena draw the i960 emits has to be C · M for the
 *    explorer's M of some part and one view matrix C per frame — verify-stage's
 *    check — on a replay of the coprocessor that applies every command that
 *    writes the matrix (lib/cop-replay.mjs), not just push/pop, translate, scale
 *    and the three angles. What this adds is the clocks. The explorer runs every
 *    animation off one frame number; the board runs some off frame_counter and
 *    some off the age object_cont keeps in each stage object, which starts with
 *    the round. Each part may take frame_counter or a recorded age (at one or two
 *    a frame, a few frames either way) — and has to keep the same one for the
 *    whole capture. A part that is exact on one clock throughout has the board's
 *    rate and shape.
 *
 * 2. The flight. Where the world moves (Flying Carpet, Canyon Cruise, Giant Wing)
 *    the position, heading, pitch and roll the object wrote are held against
 *    carpetAt / canyonAt / giantWingRoll at that object's age.
 *
 * 3. What the coprocessor draws. Fn_put_poly copies the current matrix into the
 *    display list the renderer walks. Placement grades the i960's commands and
 *    never sees that, so the emulator's own words are read out of bufferram and
 *    held against a float32 replay with the chip's sine table.
 *
 * 4. Texture animation: the aurora's and the Death Egg floor's texture points
 *    (tpd_move) and the sea's and river's lumabase (transmap_change), found in
 *    what the i960 writes to geometry program memory.
 *
 * Where display.js leaves out something the i960 listing does, the part is tried
 * both as the explorer builds it and with the listing's correction, and the
 * report says which parts only match corrected — so an explorer fix shows up as
 * a correction no longer being needed. Two parts have one degree of freedom the
 * explorer cannot supply: the Flying Carpet's corner posts face the camera, and
 * the sphynx head aims at the fighters.
 *
 *   node tools/grade-stages.mjs                       # capture and grade stages 0..14
 *   node tools/grade-stages.mjs --stages 1,4,7 --frames 600
 *   node tools/grade-stages.mjs --stages 4 --frames 2000 --no-blocks   # a whole canyon run
 *   node tools/grade-stages.mjs --no-capture --out <dir>
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom, loadRom } from './lib/rom.mjs';
import { nc, noclipVersion } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { captureStage } from './lib/dl.mjs';
import { CopReplay, replayDraws, f32, GEO_PROGRAM } from './lib/cop-replay.mjs';
import {
    I4, mul, inv, maxdiff, matKey, rotY, boardMatrix, describeResidual, ANGLE_UNIT,
} from './lib/matrix.mjs';

const args = parseArgs(['stages', 'frames', 'jobs', 'port', 'out', 'grade-one', 'slot']);
const OUT = args.str('out', path.join(os.tmpdir(), 'm2hle-stages'));
const EPS = 1e-5;
const s16 = (v) => (v << 16) >> 16;

const FLYING_CARPET = 1, CANYON_CRUISE = 4, DYNAMITE_PLANT = 6, GIANT_WING = 7;
const SPHYNX_HEAD = 322;
const NAMES = {
    0: 'South Island', 1: 'Flying Carpet', 2: 'Aurora Icefield', 3: 'Mushroom Hill', 4: 'Canyon Cruise',
    5: 'Casino Night', 6: 'Dynamite Plant', 7: 'Giant Wing', 8: 'Death Egg', 9: "Death Egg's Eye",
    10: 'Final Eggman Boss', 13: 'South Island (alt)', 14: 'South Island (ADV_MOVIE)',
};
const stageName = (n) => NAMES[n] ? `stage ${n} (${NAMES[n]})` : `stage ${n}`;

/*
 * What display.js leaves out, each read off the i960 listing. `fix` takes the
 * part's own ops (its world prologue already taken off) and returns them
 * corrected, or null where it does not apply.
 */
const S16 = ['s', [1.6, 1.6, 1.6]];
const OMISSIONS = [
    {
        slot: FLYING_CARPET, id: 'flame-base3x3',
        what: 'pole_disp resets the 3x3 (0x08001010) after lifting each flame 4.1, before its flicker scale',
        applies: (e) => e.layer === 'poles' && e.anim,
        fix: (own) => (own.some((op) => op[0] === 'b') ? null : [...own.slice(0, 3), ['b'], ...own.slice(3)]),
    },
    {
        slot: FLYING_CARPET, id: 'sphynx-inner8',
        what: 'draw_sphynx_head loads inner slot 8 (the arena frame, scaled 1.6) over its prologue before the translate, so the head is drawn at 1.6',
        applies: (e) => e.model === SPHYNX_HEAD,
        fix: (own) => (own[0]?.[0] === 't' ? [own[0], S16, ...own.slice(1)] : null),
    },
    {
        slot: GIANT_WING, id: 'plane-inner8',
        what: 'giant_wing_disp draws the body, haze and clouds from inner slot 8 (0x44(8)): the rolled arena frame at 1.6',
        applies: (e) => e.layer === 'objects' && [2947, 3321, 3322, 3086, 3087, 3673].includes(e.model),
        fix: (own) => [S16, ...own], world: true,
    },
    {
        slot: null, id: 'cage-shake',
        what: 'cage_clip_m opens each wall with a translate of dword_903D0[word_50A1E8[wall]], the shake a fighter thrown into it sets off',
        applies: (e) => e.layer === 'cage' && opsHas(e, (op) => op[0] === 'r'),
        fix: (own, ctx) => {
            const r = own.findIndex((op) => op[0] === 'r');
            const wall = ((Math.round(own[r][1] / 90) % 4) + 4) % 4;
            const z = ctx.cageShake?.(wall);
            return z ? [...own.slice(0, r + 1), ['t', [0, 0, -z]], ...own.slice(r + 1)] : null;
        },
    },
    {
        slot: DYNAMITE_PLANT, id: 'gear-phase',
        what: 'slot6_obj0_init starts the second gear\'s accumulator (+0x42) at 0x800 and the first at 0',
        applies: (e) => e.model === 2265 && opsHas(e, (op) => op[0] === 's' && op[1][0] === 1.2),
        fix: (own) => own.map((op) => (op[0] === 'rz' ? ['rz', op[1] - 0x800 * ANGLE_UNIT] : op)),
    },
];
function opsHas(e, pred) { const ops = typeof e.ops === 'function' ? e.ops(0) : e.ops; return ops.some(pred); }

/*
 * State a round reached by pinning stage_num skips. The Final Eggman Boss is
 * only ever entered from the Death Egg's Eye, whose transition ends with
 * bossm_cont setting bit 31 of 0x500498; sub_2731C draws the hangar iris only
 * with it set (bbc 31 at 0x27D80) and display.js builds that state. Pinned
 * straight to slot 10 the bit is clear and the iris is never drawn.
 */
const BOSSM_STATE = 0x00500498;
const PREPARE = {
    10: async (emu) => {
        const v = (await emu.readMemory(BOSSM_STATE, 4)).readUInt32LE(0);
        const b = Buffer.alloc(4); b.writeUInt32LE((v | 0x80000000) >>> 0);
        await emu.writeMemory(BOSSM_STATE, b);
        return `set bit 31 of 0x500498 (was 0x${v.toString(16)}): the Death Egg's Eye transition the pin skipped`;
    },
};

/* =========================================================================== */
/* grading one capture (run in a child process)                                */
/* =========================================================================== */

async function gradeOne(dir, slot) {
    const scene = JSON.parse(fs.readFileSync(path.join(dir, 'fight-scene.json'), 'utf8'));
    const meta = JSON.parse(fs.readFileSync(path.join(dir, 'fight.json'), 'utf8'));
    const bin = fs.readFileSync(path.join(dir, 'fight.bin'));
    const marks = meta.marks;
    const { rom } = await loadRom();
    const D = await nc('display.js');
    const { readStageTable } = await nc('stages.js');
    const { readModelEntry } = await nc('romset.js');

    const byEntry = new Map(), byMesh = new Map();
    for (let i = 0; i < rom.game.modelTable.count; i++) {
        const e = readModelEntry(rom, i);
        if (!e.meshPtr) continue;
        const k = `${e.uvPtr}/${e.matPtr}/${e.meshPtr}`;
        if (!byEntry.has(k)) byEntry.set(k, i);
        if (!byMesh.has(e.meshPtr)) byMesh.set(e.meshPtr, { model: i, uvPtr: e.uvPtr, matPtr: e.matPtr });
    }
    const stage = readStageTable(rom)[slot];
    const frameTables = D.readFrameTables(rom);
    const list = D.buildStageDisplayList(stage, frameTables);
    const probe = (name) => 2 + scene.probes.findIndex(([n]) => n === name);
    const ix = { fc: probe('frameCounter'), sky: probe('skyAngle'), camYaw: probe('camYaw'),
                 ages: scene.objects.map((_, j) => probe(`obj${j}Age`)) };

    const result = { slot, loaded: scene.loaded, ambiguous: scene.ambiguous, frames: marks.length - 1,
                     objects: scene.objects.map((o) => o.disp.toString(16)) };

    const stageModels = new Set();
    for (const e of list) {
        if (e.anim) for (let f = 0; f < 4096; f++) stageModels.add(D.frameModel(e.anim, f));
        else stageModels.add(e.model);
    }
    stageModels.delete(0);     /* the empty table entry: an animation frame that draws nothing */

    result.placement = placement();
    if ([FLYING_CARPET, CANYON_CRUISE, GIANT_WING].includes(slot) && scene.objects.length) result.flight = flight();
    if (scene.blocks?.length) result.drawn = drawn();
    result.texture = texture();
    return result;

    /* ---- 1. placement on measured clocks ---------------------------------- */
    function placement() {
        const omissions = OMISSIONS.filter((o) => o.slot === null || o.slot === slot);
        const shakeIx = [0, 1, 2, 3].map((w) => scene.probes.findIndex(([n]) => n === `cageShake${w}`));
        const shakeTable = (i) => rom.mainCpuView.getFloat32(0x903d0 + i * 8, true);   /* board.mjs CAGE_SHAKE_TABLE */
        let ctx = {};
        const moving = D.stageWorldFrame(stage, 100, frameTables) !== null;
        const skyDrifts = (stage.flags >>> 0x1b) & 1;
        const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

        function partAt(e, f, om) {
            const pro = moving ? (D.stageWorldFrame(stage, f, frameTables) ?? []) : [];
            let ops = D.opsAt(e, f);
            let world = pro.length > 0 && same(ops.slice(0, pro.length), pro);
            let half = false;
            if (!world && slot === CANYON_CRUISE && e.layer === 'sky' && pro.length) {
                /* doom_cnt skips the prologue's translate on this stage. */
                const rot = pro.filter((op) => op[0] !== 't');
                if (same(ops.slice(0, rot.length), rot)) { half = true; ops = ops.slice(rot.length); }
            }
            if (world) ops = ops.slice(pro.length);
            if (om) {
                const fixed = om.fix(ops, ctx);
                if (!fixed) return null;
                ops = fixed;
                if (om.world) world = pro.length > 0;
            }
            /* Up to a Fn_base_3x3 the ops compose with the view; the 3x3 of that is then
             * the identity, and what follows applies to it. */
            const b = ops.findIndex((op) => op[0] === 'b');
            return {
                model: e.anim ? D.frameModel(e.anim, f) : e.model,
                m: boardMatrix(b >= 0 ? ops.slice(0, b) : ops),
                post: b >= 0 ? boardMatrix(ops.slice(b + 1)) : null,
                world, half,
            };
        }

        const replay = new CopReplay();
        const frames = [...replayDraws(bin, marks, { replay, byEntry, byMesh })];
        const partClocks = list.map(() => null), partDrawn = list.map(() => 0);
        /* Per part: frames it matched as the explorer builds it, and frames it
         * needed a correction for (by correction). The clock is kept apart from
         * the correction, since a wall at rest and a wall shaking are one clock. */
        const partPlain = list.map(() => 0), partCorr = list.map(() => new Map());
        const note = (pi, exact) => {
            const plain = exact.filter((c) => !c.label.includes('*'));
            if (plain.length) partPlain[pi]++;
            else {
                const id = exact[0].label.split('*')[1];
                partCorr[pi].set(id, (partCorr[pi].get(id) ?? 0) + 1);
            }
            const ls = new Set((plain.length ? plain : exact).map((c) => c.label.split('*')[0]));
            partClocks[pi] = partClocks[pi] ? new Set([...partClocks[pi]].filter((l) => ls.has(l))) : ls;
        };
        const worldErr = new Map();
        const out = { draws: 0, exact: 0, billboards: 0, aimed: 0, degenerate: 0, unmatched: [], aim: [] };

        for (const [fi, draws] of frames.entries()) {
            const mark = marks[fi];
            ctx = {
                cageShake: shakeIx[0] >= 0
                    ? (w) => { const i = mark[2 + shakeIx[w]] & 0xffff; return i < 64 ? shakeTable(i) : 0; }
                    : null,
            };
            const tally = new Map();
            for (const d of draws) {
                if (!stageModels.has(d.model)) continue;
                const k = matKey(d.base); const t = tally.get(k) ?? { m: d.base, n: 0 }; t.n++; tally.set(k, t);
            }
            let C = null, bestN = 0;
            for (const t of tally.values()) if (t.n > bestN) { bestN = t.n; C = t.m; }
            if (!C || !inv(C)) { out.degenerate++; continue; }
            const seen = new Set();
            const stageDraws = draws.filter((d) => {
                if (d.model === 0 || matKey(d.base) !== matKey(C)) return false;
                const k = `${d.model}|${matKey(d.m)}`;
                if (seen.has(k)) return false;      /* a piece two scenery runs both hold, drawn twice */
                seen.add(k);
                return true;
            });

            const clocks = [['fc', mark[ix.fc]]];
            ix.ages.forEach((i, j) => {
                const age = mark[i] & 0xffff;
                for (const r of [1, 2]) for (let d = -3; d <= 3; d++) clocks.push([`obj${j}/${r}${d >= 0 ? '+' : ''}${d}`, Math.floor(age / r) + d]);
            });

            const cand = [];
            for (const [pi, e] of list.entries()) {
                const oms = [null, ...omissions.filter((o) => o.applies(e))];
                for (const om of oms) {
                    for (const [label, f] of clocks) {
                        if (f < 0) continue;
                        const p = partAt(e, f, om);
                        if (p) cand.push({ pi, label: om ? `${label}*${om.id}` : label, f, ...p });
                    }
                }
            }

            let worldBase = C, halfBase = C;
            if (moving) {
                const votes = new Map();
                for (const g of stageDraws) for (const c of cand) {
                    if (!c.world || c.model !== g.model) continue;
                    const vi = inv(list[c.pi].layer === 'sky' ? mul(rotY(skyTurn(c)), c.m) : c.m);
                    if (!vi) continue;
                    const w = mul(g.m, vi), k = matKey(w);
                    const v = votes.get(k) ?? { m: w, n: 0 }; v.n++; votes.set(k, v);
                }
                let best = null;
                for (const v of votes.values()) if (!best || v.n > best.n) best = v;
                if (best) {
                    worldBase = best.m;
                    const P = mul(inv(C), worldBase);
                    halfBase = mul(C, [P[0], P[1], P[2], 0, P[4], P[5], P[6], 0, P[8], P[9], P[10], 0, 0, 0, 0, 1]);
                    for (const [label, f] of clocks) {
                        const pro = D.stageWorldFrame(stage, f, frameTables);
                        if (pro) worldErr.set(label, Math.max(worldErr.get(label) ?? 0, maxdiff(P, boardMatrix(pro))));
                    }
                }
            }
            function skyTurn(c) { return (mark[ix.sky] - (skyDrifts ? 2 * c.f : 0)) * ANGLE_UNIT; }
            const Crot = [C[0], C[1], C[2], 0, C[4], C[5], C[6], 0, C[8], C[9], C[10], 0, 0, 0, 0, 1];
            const pre = (c) => {
                const e = list[c.pi];
                let b = e.backdrop ? Crot : c.world ? worldBase : c.half ? halfBase : C;
                return e.layer === 'sky' ? mul(b, rotY(skyTurn(c))) : b;
            };
            const byModel = new Map();
            for (const c of cand) {
                c.pred = mul(pre(c), c.m);
                if (c.post) c.pred = mul([1, 0, 0, c.pred[3], 0, 1, 0, c.pred[7], 0, 0, 1, c.pred[11], 0, 0, 0, 1], c.post);
                if (!byModel.has(c.model)) byModel.set(c.model, []);
                byModel.get(c.model).push(c);
            }

            const claimed = new Set();
            const residual = (c, g) => {
                const Dm = mul(inv(c.m) ?? I4(), mul(inv(pre(c)) ?? I4(), g.m));
                const unit = [0, 1, 2].every((i) => Math.abs(Math.hypot(Dm[i], Dm[4 + i], Dm[8 + i]) - 1) < 1e-4);
                const still = [Dm[3], Dm[7], Dm[11]].every((v) => Math.abs(v) < 1e-4);
                const aboutY = unit && still && [Dm[1], Dm[4], Dm[6], Dm[9]].every((v) => Math.abs(v) < 1e-4);
                return { Dm, rotation: unit && still, aboutY, yaw: Math.atan2(-Dm[2], Dm[0]) * 180 / Math.PI };
            };
            for (const g of stageDraws) {
                const pool = byModel.get(g.model) ?? [];
                let best = null;
                for (const c of pool) {
                    if (claimed.has(c.pi)) continue;
                    c.err = maxdiff(g.m, c.pred);
                    if (!best || c.err < best.err) best = c;
                }
                if (!best) continue;
                out.draws++;
                const exact = pool.filter((c) => c.pi === best.pi && c.err <= EPS);
                if (exact.length) {
                    claimed.add(best.pi); out.exact++; partDrawn[best.pi]++;
                    note(best.pi, exact);
                    continue;
                }
                /* The two freedoms, each tried at every clock the part has. */
                let freed = null;
                for (const c of pool.filter((c) => !claimed.has(c.pi) && !c.post).sort((a, b) => a.err - b.err)) {
                    const r = residual(c, g);
                    if (c.model === SPHYNX_HEAD && r.rotation) { freed = { c, kind: 'aimed', r }; break; }
                    if (slot === FLYING_CARPET && list[c.pi].layer === 'poles' && r.aboutY) {
                        const cam = s16(mark[ix.camYaw]) * ANGLE_UNIT;
                        if (Math.abs(((r.yaw - cam + 540) % 360) - 180) < 1e-3) { freed = { c, kind: 'billboard', r }; break; }
                    }
                }
                if (freed) {
                    const fpi = freed.c.pi;
                    claimed.add(fpi); partDrawn[fpi]++;
                    note(fpi, pool.filter((c) => c.pi === fpi && !c.post).filter((c) => {
                        const r = residual(c, g); return freed.kind === 'aimed' ? r.rotation : r.aboutY;
                    }));
                    if (freed.kind === 'aimed') { out.aimed++; out.aim.push(freed.r.yaw); } else out.billboards++;
                    continue;
                }
                claimed.add(best.pi);
                out.unmatched.push({ fi, pi: best.pi, model: g.model, layer: list[best.pi].layer, label: best.label,
                                     residual: describeResidual(residual(best, g).Dm) });
            }
        }

        /* Per part: the clocks it was exact on for the whole capture. */
        const allClocks = 1 + 14 * ix.ages.length;
        const clockOf = new Map(), onlyCorrected = new Map(), sometimes = new Map(), switched = [];
        const probeF = [0, 1, 2, 3, 7, 31, 64, 100, 255, 256, 511, 700, 1023, 1500, 2047, 3000];
        const sig = (e, f) => JSON.stringify([e.anim ? D.frameModel(e.anim, f) : e.model, D.opsAt(e, f)]);
        /* A texture scroll or a luma band animates a part whose geometry stands still. */
        const animated = (e) => Boolean(e.scroll || e.band) || probeF.some((f) => sig(e, f) !== sig(e, 0));
        for (const [pi, e] of list.entries()) {
            if (!partDrawn[pi]) continue;
            const ls = [...(partClocks[pi] ?? [])];
            if (!ls.length) { switched.push(`#${pi} ${e.layer}:${e.model}`); continue; }
            for (const [id, n] of partCorr[pi]) {
                if (!partPlain[pi]) onlyCorrected.set(id, (onlyCorrected.get(id) ?? 0) + 1);
                else sometimes.set(id, (sometimes.get(id) ?? 0) + n);
            }
            if (!animated(e)) continue;
            const use = ls;
            const k = ix.ages.length && new Set(use).size >= allClocks ? 'any'
                : [...new Set(use)].sort((a, b) => a.length - b.length).slice(0, 3).join(' | ');
            if (!clockOf.has(k)) clockOf.set(k, []);
            clockOf.get(k).push(`${e.layer}:${e.anim ? 'anim' : e.model}`);
        }
        const unseen = list.filter((e, pi) => !partDrawn[pi] && animated(e)).map((e) => `${e.layer}:${e.anim ? 'anim' : e.model}`);
        const groups = new Map();
        for (const u of out.unmatched) {
            const k = `${u.layer} model ${u.model}`;
            const g = groups.get(k) ?? { n: 0, first: u, last: u }; g.n++; g.last = u; groups.set(k, g);
        }
        return {
            draws: out.draws, exact: out.exact, billboards: out.billboards, aimed: out.aimed, degenerate: out.degenerate,
            aim: out.aim.length ? [Math.min(...out.aim), Math.max(...out.aim)] : null,
            unmatched: [...groups].map(([k, g]) => `${k}: ${g.n} draws between marks ${g.first.fi} and ${g.last.fi} ` +
                `(frame_counter ${marks[g.first.fi][ix.fc]}..${marks[g.last.fi][ix.fc]}), e.g. ${g.first.residual}`),
            switched, unseen,
            clocks: [...clockOf].map(([k, parts]) => ({ clock: k, parts: [...new Set(parts)] })),
            omissions: [...onlyCorrected].map(([id, n]) => ({ id, parts: n, what: OMISSIONS.find((o) => o.id === id).what })),
            states: [...sometimes].map(([id, n]) => ({ id, draws: n, what: OMISSIONS.find((o) => o.id === id).what })),
            world: moving ? [...worldErr].sort((a, b) => a[1] - b[1]).slice(0, 1).map(([l, e]) => ({ clock: l, err: e }))[0] : null,
        };
    }

    /* ---- 2. the flight ------------------------------------------------------ */
    function flight() {
        const at = (m) => ({
            age: m[ix.ages[0]] & 0xffff,
            pos: ['stageX', 'stageY', 'stageZ'].map((n) => f32(m[probe(n)])),
            angX: s16(m[probe('carpetAngX')]), yaw: s16(m[probe('carpetHeading')]), roll: s16(m[probe('carpetRoll')]),
        });
        const model = (f) => (slot === FLYING_CARPET ? D.carpetAt(f)
            : slot === CANYON_CRUISE ? D.canyonAt(frameTables.canyon, f)
                : { pos: [0, 0, 0], yaw: 0, pitch: 0, roll: D.giantWingRoll(f) });
        /* object_cont steps the age after the routine flies the frame, so the
         * state a mark reads was flown at the age before. The age is carried on
         * continuously from the first mark rather than read at each: Canyon
         * Cruise's continuation snaps its counter back at the end of a run after
         * flying the frame, and the explorer's canyonClock does that loop itself. */
        let pos = 0, yaw = 0, pitch = 0, roll = 0;
        const age0 = at(marks[0]).age;
        for (const [i, m] of marks.entries()) {
            const b = at(m), e = model(age0 + i - 1);
            if (slot === GIANT_WING) { roll = Math.max(roll, Math.abs(s16(e.roll - b.roll))); continue; }
            pos = Math.max(pos, ...e.pos.map((v, i) => Math.abs(v - b.pos[i])));
            yaw = Math.max(yaw, Math.abs(s16(Math.round(e.yaw) - b.yaw)));
            pitch = Math.max(pitch, Math.abs(s16(Math.round(e.pitch) - b.angX)));
        }
        const [b0, b1] = [at(marks[0]), at(marks[marks.length - 1])];
        return { pos, yaw, pitch, roll, ages: [b0.age, b1.age] };
    }

    /* ---- 3. what the coprocessor hands the renderer ------------------------- */
    function drawn() {
        const blocks = fs.readFileSync(path.join(dir, 'fight.blocks.bin'));
        const BLOCK = scene.blocks[0][1];
        const replay = new CopReplay({ float32: true, copro: rom.coproView });
        let compared = 0, differ = 0, tainted = 0, worst = 0, base3x3 = 0;
        const bad = new Map();
        for (const draws of replayDraws(bin, marks, { replay, byEntry, byMesh })) {
            for (const d of draws) {
                if (d.via !== 'copro') continue;
                if (d.tainted) { tainted++; continue; }
                const closing = d.frame + 1;
                if ((closing + 1) * BLOCK > blocks.length) continue;
                const at = closing * BLOCK + (d.listOffset & (BLOCK - 1));
                const w = (k) => blocks.readUInt32LE(at + k * 4);
                compared++;
                if (d.base3x3) base3x3++;
                /* The record Fn_put_poly writes: 0x05800B0B, the slot's 12 words, 0x00800101, tpa, tha, oba, count. */
                let ok = w(0) === 0x05800b0b && w(13) === 0x00800101 && w(16) === d.oba;
                for (let k = 0; k < 12 && ok; k++) {
                    const e = Math.abs(f32(w(1 + k)) - d.slot[k]);
                    if (e > 2e-5 + 2e-6 * Math.abs(d.slot[k])) ok = false;
                    else worst = Math.max(worst, e);
                }
                if (!ok) {
                    differ++;
                    const k = `model ${d.model}${d.base3x3 ? ' after Fn_base_3x3' : ''}`;
                    bad.set(k, (bad.get(k) ?? 0) + 1);
                }
            }
        }
        return { compared, differ, tainted, worst, base3x3, bad: [...bad].map(([k, n]) => `${k}: ${n}`) };
    }

    /* ---- 4. texture animation ------------------------------------------------ */
    function texture() {
        const geo = [];
        for (let fi = 0; fi + 1 < marks.length; fi++) {
            const words = [];
            for (let k = marks[fi][1]; k < marks[fi + 1][1]; k++) {
                const o = bin.readUInt32LE(k * 8);
                if (o >= GEO_PROGRAM[0] && o < GEO_PROGRAM[1]) words.push(bin.readUInt32LE(k * 8 + 4));
            }
            geo.push({ fc: marks[fi][ix.fc], words });
        }
        const rows = [];
        /* display.js AURORA_POINTS / AURORA_SCROLL and BOSS_FLOOR_POINTS / BOSS_FLOOR_SCROLL. */
        const TPD = {
            2: { what: "the aurora's texture points", at: 0x758fc, scroll: { axis: 'v', step: -4, shift: 0, mask: 0x7ff } },
            8: { what: "the Death Egg floor's texture points", at: 0x72fc4, scroll: { axis: 'u', step: 32, shift: 3, mask: 0x7ff } },
        };
        if (TPD[slot]) {
            const t = TPD[slot], pts = D.readTexturePoints(rom, t.at), walk = t.scroll.axis === 'v' ? 0 : 1;
            const deltas = new Map(), users = new Map();
            let found = 0, drawnFrom = 0;
            for (const { fc, words } of geo) {
                let off = null, at = -1;
                for (let i = 0; i + pts.length <= words.length && off === null; i++) {
                    let o = null, ok = true;
                    for (let k = 0; k < pts.length && ok; k++) {
                        const w = words[i + k] & 0xffff;
                        if (k % 2 !== walk) { ok = w === pts[k]; continue; }
                        const d = (w - pts[k]) & 0xffff;
                        if (o === null) o = d; else ok = d === o;
                    }
                    if (ok) { off = o; at = i; }
                }
                if (off === null) continue;
                found++;
                const k = nearest((d) => (D.frameScroll(t.scroll, fc + d) * 8) & 0xffff, off);
                deltas.set(k, (deltas.get(k) ?? 0) + 1);
                /* The upload the block went out in: command 4's (address, count), the
                 * address in geometrizer texture RAM (bit 23). An object that draws
                 * the scroll names that address as its texture points. */
                let addr = null;
                for (let h = at - 2; h >= Math.max(0, at - 0x1000) && addr === null; h--) {
                    if ((words[h] & 0xff800000) === 0x00800000 && at < h + 2 + words[h + 1]) addr = (words[h] + (at - h - 2)) >>> 0;
                }
                if (addr === null) continue;
                let used = false;
                for (let i = 0; i + 2 < words.length; i++) {
                    if (i >= at - 2 && i < at + pts.length) continue;
                    const hit = words[i] === addr ? byMesh.get(words[i + 2]) : null;
                    if (hit && words[i + 1] === hit.matPtr) { used = true; users.set(hit.model, (users.get(hit.model) ?? 0) + 1); }
                }
                if (used) drawnFrom++;
            }
            rows.push({ what: t.what, frames: found, of: geo.length, deltas: [...deltas], drawnFrom,
                        users: [...users].map(([m, n]) => `model ${m} (${n} draws)`) });
        }
        if ([0, 4, 13, 14].includes(slot)) {
            /* transmap_change's quads: 0x40DA, the lumabase, 0x0A0C, 0x03C0. */
            const SEA = { first: 7, count: 64, shift: 1 };
            const deltas = new Map();
            let found = 0, mixed = 0;
            for (const { fc, words } of geo) {
                const bands = new Set();
                for (let i = 0; i + 4 <= words.length; i++) {
                    if ((words[i] & 0xffff) === 0x40da && (words[i + 2] & 0xffff) === 0x0a0c && (words[i + 3] & 0xffff) === 0x03c0) bands.add(words[i + 1] & 0xffff);
                }
                if (!bands.size) continue;
                found++;
                if (bands.size > 1) { mixed++; continue; }
                const k = nearest((d) => D.frameBand(SEA, fc + d), [...bands][0]);
                deltas.set(k, (deltas.get(k) ?? 0) + 1);
            }
            rows.push({ what: 'the water\'s lumabase', frames: found, of: geo.length, deltas: [...deltas], mixed });
        }
        return rows;
    }
    function nearest(fn, want) {
        for (const d of [0, -1, 1, -2, 2, -3, 3, -4, 4]) if (fn(d) === want) return d;
        return null;
    }
}

if (args.str('grade-one')) {
    const r = await gradeOne(args.str('grade-one'), args.num('slot'));
    process.stdout.write('\n@@RESULT ' + JSON.stringify(r) + '\n');
    process.exit(0);
}

/* =========================================================================== */
/* capture, grade, report                                                      */
/* =========================================================================== */

const rep = new Report('grade-stages — every arena\'s animations and draw routines, emulator vs explorer');
const stages = (args.str('stages') ?? '0,1,2,3,4,5,6,7,8,9,10,11,12,13,14').split(',').map(Number);
const jobs = args.num('jobs', 3);
const frames = args.num('frames', 180);
const port0 = args.num('port', 7180);
const dirOf = (n) => path.join(OUT, `stage${String(n).padStart(2, '0')}`);
rep.note(`explorer ${noclipVersion()}, captures in ${OUT}`);

async function pool(items, n, fn) {
    const queue = [...items], running = [];
    const results = new Map();
    for (let slotIx = 0; slotIx < n; slotIx++) {
        running.push((async () => {
            while (queue.length) {
                const item = queue.shift();
                results.set(item, await fn(item, slotIx));
            }
        })());
    }
    await Promise.all(running);
    return results;
}

if (!args.bool('no-capture')) {
    const rom = findRom().primary;
    const failures = await pool(stages, jobs, async (n, lane) => {
        const emu = await M2Hle.launch({ rom, port: port0 + lane });
        try {
            await emu.waitUntilRunning();
            await captureStage(emu, n, {
                out: dirOf(n), frames, prepare: PREPARE[n], blocks: !args.bool('no-blocks'),
                log: (s) => rep.note(`${stageName(n)}: ${s}`),
            });
            return null;
        } catch (e) {
            return e.message;
        } finally {
            await emu.close();
        }
    });
    for (const [n, why] of failures) if (why) rep.check(`${stageName(n)}: a round is captured`, false, why);
}

const self = fileURLToPath(import.meta.url);
const graded = await pool(stages.filter((n) => fs.existsSync(path.join(dirOf(n), 'fight-scene.json'))), Math.max(1, Math.min(os.cpus().length, 8)), (n) => new Promise((resolve) => {
    const child = spawn(process.execPath, [self, '--grade-one', dirOf(n), '--slot', String(n)], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '', err = '';
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { err += d; });
    child.on('close', () => {
        const m = out.match(/@@RESULT (.*)$/m);
        resolve(m ? JSON.parse(m[1]) : { error: (err || out).split('\n').slice(-8).join('\n') });
    });
}));

for (const n of stages) {
    const r = graded.get(n);
    const name = stageName(n);
    if (!r) { rep.skip(name, 'no capture'); continue; }
    if (r.error) { rep.check(`${name}: graded`, false, r.error); continue; }
    rep.check(`${name}: the round loaded this stage's record`, r.loaded === n || (r.ambiguous ?? []).includes(n),
              `record ${r.loaded}${r.ambiguous ? ` (shares its texture pair with ${r.ambiguous.join('/')})` : ''}; objects ${r.objects.join(', ') || 'none'}`);

    const p = r.placement;
    if (!p.draws) {
        rep.skip(`${name}: arena placement`, `${p.degenerate} frames with no stage draws — the record draws nothing`);
    } else {
        const freedoms = [p.billboards ? `${p.billboards} posts at the camera's yaw` : '', p.aimed ? `${p.aimed} head draws aimed ${p.aim.map((v) => v.toFixed(2)).join('..')}°` : '']
            .filter(Boolean).join(', ');
        rep.check(`${name}: every arena draw is an explorer part, each on one clock`, !p.unmatched.length && !p.switched.length,
                  `${p.exact} of ${p.draws} draws exact over ${r.frames} frames` + (freedoms ? `; ${freedoms}` : ''));
        for (const u of p.unmatched) rep.note(`  x ${u}`);
        for (const s of p.switched) rep.note(`  x ${s} is exact frame by frame but on no one clock`);
        for (const c of p.clocks.filter((c) => c.clock !== 'any')) rep.note(`  clock ${c.clock}: ${c.parts.join(' ')}`);
        for (const o of p.omissions) rep.note(`  explorer omission (${o.parts} parts exact only with it): ${o.what}`);
        for (const o of p.states) rep.note(`  game state the explorer has no part in (${o.draws} draws exact only with it): ${o.what}`);
        if (p.world) rep.note(`  arena frame against stageWorldFrame at ${p.world.clock}: ${p.world.err.toExponential(2)}`);
        if (p.unseen.length) rep.note(`  not exercised (animated, never drawn in this capture): ${p.unseen.join(' ')}`);
    }

    if (r.flight) {
        const f = r.flight;
        const ok = n === GIANT_WING ? f.roll === 0 : f.pos < 2e-4 && f.yaw === 0 && f.pitch <= 2;
        rep.check(`${name}: the flight is the explorer's at the object's age`, ok,
                  n === GIANT_WING ? `roll within ${f.roll} angle units, ages ${f.ages.join('..')}`
                      : `position within ${f.pos.toExponential(2)}, heading within ${f.yaw}, pitch within ${f.pitch} angle units, ages ${f.ages.join('..')}`);
    }
    if (r.drawn) {
        const d = r.drawn;
        rep.check(`${name}: the coprocessor lays down the matrices the firmware computes`, d.compared > 0 && d.differ === 0,
                  `${d.compared - d.differ} of ${d.compared} object matrices in bufferram match (worst element ${d.worst.toExponential(1)}), ` +
                  `${d.base3x3} after Fn_base_3x3; ${d.tainted} built off matrices the stream does not carry`);
        for (const b of d.bad) rep.note(`  x ${b}`);
    }
    for (const t of r.texture) {
        const exact = t.deltas.filter(([d]) => d !== null);
        const ok = t.frames === t.of && exact.length === 1 && exact[0][1] === t.frames && !t.mixed
            && (t.drawnFrom === undefined || t.drawnFrom === t.frames);
        rep.check(`${name}: ${t.what} follow the explorer`, ok,
                  `${t.frames} of ${t.of} frames; on frame_counter ${exact.map(([d, c]) => `${d >= 0 ? '+' : ''}${d} (${c} frames)`).join(', ') || 'never'}` +
                  `${t.deltas.some(([d]) => d === null) ? `, off on ${t.deltas.find(([d]) => d === null)[1]}` : ''}` +
                  (t.drawnFrom === undefined ? '' : `; drawn from the scrolled points on ${t.drawnFrom} frames by ${t.users.join(', ') || 'nothing'}`));
    }
}
rep.finish();
