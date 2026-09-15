/*
 * grade-stage.mjs — where this emulator's i960 places every arena part, against
 * where the explorer builds it.
 *
 * The check itself is the explorer toolkit's, run unchanged:
 * stf-tools/verify-stage.mjs replays a captured display list into (model,
 * matrix) draws, recovers the frame's view matrix C from the bottom of the
 * matrix stack, and requires every arena draw the game emitted to equal
 * C · M for the explorer's own M for that part — one C across every part, so a
 * shared matrix cannot absorb a mistake in any one of them. It already knows
 * the stages' three clocks, the Flying Carpet's moving frame, the sphinx head
 * that is aimed at the fighters, and which draws are the fighters rather than
 * the stage; none of that is worth a second copy.
 *
 * What this adds is the capture. The toolkit takes its display lists off MAME;
 * capture_dl takes the same stream off this emulator in the same format (see
 * lib/dl.mjs), during a round attract mode is fighting, and names the scene by
 * the stage record change_scene loaded rather than by what stage_num says.
 *
 *   node tools/grade-stage.mjs                    # capture a fight, then grade it
 *   node tools/grade-stage.mjs --capture <prefix> --stage 1
 *
 * Needs a sibling stf-tools checkout ($M2_STF_TOOLS overrides) with its
 * explorer submodule, and skips cleanly without one.
 */
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { REPO } from './lib/noclip.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { captureFight, writeStageLayout, DEFAULT_DL_OUT } from './lib/dl.mjs';

const args = parseArgs(['capture', 'stage', 'frames', 'port', 'out', 'detail']);
const rep = new Report('grade-stage — arena placement, emulator i960 vs explorer');

function findStfTools() {
    for (const base of [process.env.M2_STF_TOOLS,
                        path.resolve(REPO, '..', 'stf-tools'),
                        path.resolve(REPO, '..', '..', 'stf-tools')]) {
        if (base && fs.existsSync(path.join(base, 'verify-stage.mjs'))
                 && fs.existsSync(path.join(base, 'vendor', 'noclip', 'js', 'display.js'))) return base;
    }
    return null;
}

const tools = findStfTools();
if (!tools) {
    rep.skip('arena placement', 'no stf-tools checkout with verify-stage.mjs and its explorer submodule');
    rep.finish();
}

/* ---- the capture ----------------------------------------------------------- */

let prefix = args.str('capture');
let stage = args.num('stage', null);
if (!prefix) {
    const emu = await M2Hle.launch({ rom: findRom().primary, port: args.num('port', 7172) });
    try {
        await emu.waitUntilRunning();
        const cap = await captureFight(emu, {
            out: args.str('out', DEFAULT_DL_OUT), frames: args.num('frames', 120),
            log: (s) => rep.note(s),
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
if (stage === null) {
    rep.skip('arena placement', 'the capture does not say which stage it is; pass --stage');
    rep.finish();
}

/* ---- the toolkit's check --------------------------------------------------- */

const layout = writeStageLayout(prefix, stage);
const run = spawnSync(process.execPath,
    [path.join(tools, 'verify-stage.mjs'), path.resolve(layout), String(stage), findRom().primary], {
        cwd: tools, encoding: 'utf8', maxBuffer: 256 * 1024 * 1024,
        env: { ...process.env, ...(args.bool('detail') ? { DETAIL: '1' } : {}) },
    });
const out = (run.stdout ?? '') + (run.stderr ?? '');
if (run.status !== 0 && run.status !== 1) {
    rep.check('verify-stage ran', false, `exit ${run.status}`);
    console.log(out.split('\n').slice(-20).join('\n'));
    rep.finish();
}

const summary = [...out.matchAll(
    /^frame (\d+): (\d+)\/(\d+) arena draws at the viewer's matrix \(worst residual ([\d.e+-]+)\), (\d+) disagree, (\d+) culled/gm)];
const parts = new Map();
for (const m of out.matchAll(/^ {4}x (\S+)\s+model\s+(\d+)\s+(?:\(geo\))?\s*viewer is short by: (.*)$/gm)) {
    const k = `${m[1]} model ${m[2]}`;
    const e = parts.get(k) ?? { n: 0, first: m[3], last: m[3] };
    e.n++; e.last = m[3];
    parts.set(k, e);
}

rep.note(`stf-tools verify-stage.mjs, stage ${stage}, ${summary.length} frames`);
const drawn = summary.reduce((s, m) => s + Number(m[3]), 0);
const agree = summary.reduce((s, m) => s + Number(m[2]), 0);
const off = summary.reduce((s, m) => s + Number(m[5]), 0);
const worst = summary.reduce((w, m) => Math.max(w, Number(m[4])), 0);
const cleanFrames = summary.filter((m) => m[5] === '0').length;

rep.check('the capture replays into arena draws', summary.length > 0 && drawn > 0,
          `${drawn} arena draws over ${summary.length} frames`);
rep.check('every arena draw is at the explorer\'s matrix', run.status === 0,
          `${agree} agree (worst residual ${worst.toExponential(2)}), ${off} disagree; ` +
          `${cleanFrames} of ${summary.length} frames clean`);
for (const [k, e] of [...parts].sort((a, b) => b[1].n - a[1].n).slice(0, 12)) {
    rep.note(`  ${k}: out on ${e.n} draws — first ${e.first}, last ${e.last}`);
}
rep.finish();
