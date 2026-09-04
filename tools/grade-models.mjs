/*
 * grade-models.mjs — hold the emulator's polygon decoder against the explorer's.
 *
 * The index-array decoder is the part of this emulator with the most
 * hand-derived rules in it. CLAUDE.md calls them load-bearing and says not to
 * re-derive them: the iFlag table, the A-B-D-C quad winding, the `i < n_idx - 8`
 * face loop that stays two groups behind the tail, the (x, y, -z) read, and the
 * 0x45B4 connectivity mask that was found by brute force rather than reasoned
 * out. Every one of those is a place a plausible-looking change silently
 * degrades thousands of models.
 *
 * So this sweeps the whole model table through both decoders and compares the
 * geometry. Two implementations, written from the same reverse engineering but
 * separately, over all 5103 entries — a rule that is wrong in one of them shows
 * up as a Jaccard below 1 on the models that exercise it, and the report names
 * the worst ones rather than just the average.
 *
 * Geometry only, deliberately. Colour, texture tile and UV depend on what the
 * running game has uploaded into texture RAM; grading the decoder should not be
 * measuring that. Positions are a pure function of the ROM, so this check needs
 * no scene, no driving and no capture — which is what makes it the one to run
 * after touching geo3d.h.
 *
 *   node tools/grade-models.mjs                # the whole table
 *   node tools/grade-models.mjs --first 500 --count 100
 *   node tools/grade-models.mjs --worst 20     # list the worst disagreements
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { M2Hle } from './lib/m2hle.mjs';
import { loadRom, findRom } from './lib/rom.mjs';
import { nc, noclipVersion } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['first', 'count', 'port', 'worst', 'quant', 'dump']);
const rep = new Report('grade-models — polygon decoder, emulator vs explorer');

/* Positions are floats out of both decoders; comparing them exactly would be
 * grading the order of two multiplications. A thousandth of a world unit is far
 * below anything the format can express — vertices are 16-bit — so this
 * separates "the same vertex" from "a different one" without being a tolerance
 * that hides a real disagreement. */
const QUANT = args.num('quant', 1000);

/* ---- 1. the emulator's answer ------------------------------------------- */

const dumpPath = args.str('dump', path.join(os.tmpdir(), `m2hle-models-${process.pid}.bin`));
const keepDump = args.str('dump') !== null;
const port = args.num('port', 7172);
const attach = args.bool('attach');

const emu = attach
    ? await M2Hle.attach({ port })
    : await M2Hle.launch({ rom: findRom().primary, port, run: false });

try {
    /* No frames needed: dump_model decodes out of the ROM regions, not out of
     * the running machine, so the emulator only has to have finished loading a
     * set — but it does have to have finished. */
    const st = await emu.waitForRom();
    rep.note(`emulator: profile ${st.profile}, ROM loaded`);
    const r = await emu.rpc('dump_model', {
        model: args.num('first', 0),
        count: args.num('count', 100000),   /* clamped to the table by the bridge */
        path: dumpPath,
    });
    rep.note(`emulator: ${r.count} entries decoded, ${r.nonempty} with geometry, ` +
             `${r.tris} triangles`);
} finally {
    await emu.close({ kill: !attach });
}

const models = readDump(dumpPath);
if (!keepDump) fs.rmSync(dumpPath, { force: true });

/* ---- 2. the explorer's answer ------------------------------------------- */

const { rom, primary } = await loadRom();
const { decodeModel } = await nc('model.js');
rep.note(`explorer ${noclipVersion()}, rom ${path.basename(primary)}`);

/* ---- 3. compare ---------------------------------------------------------- */

let bothEmpty = 0, bothGeometry = 0, onlyEmu = 0, onlyExplorer = 0;
let exactTris = 0, sameCount = 0;
let interTotal = 0, unionTotal = 0;
const perModel = [];

for (const { index, tris } of models) {
    const theirs = decodeModel(rom, index);
    const theirTris = theirs ? theirs.positions.length / 9 : 0;
    const ourTris = tris.length / 9;

    if (!ourTris && !theirTris) { bothEmpty++; continue; }
    if (!theirTris) { onlyEmu++; continue; }
    if (!ourTris) { onlyExplorer++; continue; }
    bothGeometry++;
    if (ourTris === theirTris) sameCount++;

    const a = triSet(tris), b = triSet(theirs.positions);
    let inter = 0;
    for (const k of a.keys()) if (b.has(k)) inter += Math.min(a.get(k), b.get(k));
    const union = sum(a) + sum(b) - inter;
    interTotal += inter;
    unionTotal += union;
    const j = union ? inter / union : 1;
    if (j === 1) exactTris++;
    else perModel.push({ index, j, ourTris, theirTris });
}

const measured = bothGeometry;
rep.note(`${models.length} entries: ${bothGeometry} carry geometry in both, ` +
         `${bothEmpty} empty in both, ${onlyEmu} only here, ${onlyExplorer} only there`);

rep.check('every entry decodes the same way (empty or not)',
          onlyEmu === 0 && onlyExplorer === 0,
          onlyEmu || onlyExplorer
              ? `${onlyEmu} decode here but not there, ${onlyExplorer} the other way`
              : `${models.length} entries agree`);

rep.check('triangle counts agree', sameCount === measured,
          `${sameCount} of ${measured} models`);

const J = unionTotal ? interTotal / unionTotal : 1;
rep.check('geometry is identical (Jaccard = 1)', J === 1,
          `J = ${J.toFixed(6)} over ${unionTotal} triangles; ` +
          `${exactTris} of ${measured} models exact`);

if (perModel.length) {
    const worst = args.num('worst', 10);
    perModel.sort((x, y) => x.j - y.j);
    rep.note(`worst ${Math.min(worst, perModel.length)} of ${perModel.length} disagreeing models:`);
    for (const m of perModel.slice(0, worst)) {
        rep.note(`  model ${m.index}: J = ${m.j.toFixed(4)}, ` +
                 `${m.ourTris} tris here vs ${m.theirTris} there`);
    }
}

rep.finish();

/* ---- helpers ------------------------------------------------------------- */

/**
 * A triangle as a key that does not depend on which corner either decoder
 * started at, so a winding or rotation difference is not counted as a different
 * triangle — that is a separate question from whether the same surface came
 * out, and this check is about the surface.
 */
function triSet(pos) {
    const m = new Map();
    for (let i = 0; i < pos.length; i += 9) {
        const v = [];
        for (let k = 0; k < 3; k++) {
            v.push([Math.round(pos[i + k * 3] * QUANT),
                    Math.round(pos[i + k * 3 + 1] * QUANT),
                    Math.round(pos[i + k * 3 + 2] * QUANT)].join(','));
        }
        const key = v.sort().join('|');
        m.set(key, (m.get(key) ?? 0) + 1);
    }
    return m;
}

function sum(m) { let t = 0; for (const v of m.values()) t += v; return t; }

/** Parse what dump_model wrote — see the comment on it in mcp_bridge.h. */
function readDump(file) {
    const b = fs.readFileSync(file);
    if (b.toString('latin1', 0, 4) !== 'M2MD') throw new Error(`${file} is not a model dump`);
    const version = b.readUInt32LE(4);
    if (version !== 1) throw new Error(`model dump version ${version}, expected 1`);
    const count = b.readUInt32LE(12);
    const out = [];
    let o = 16;
    for (let i = 0; i < count; i++) {
        const index = b.readUInt32LE(o);
        const n = b.readUInt32LE(o + 4);
        o += 8;
        const floats = n * 9;
        const tris = new Float32Array(floats);
        for (let k = 0; k < floats; k++) tris[k] = b.readFloatLE(o + k * 4);
        o += floats * 4;
        out.push({ index, tris });
    }
    return out;
}
