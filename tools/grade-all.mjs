/*
 * grade-all.mjs — every grader, one capture, one verdict.
 *
 * The graders that need a scene share one capture rather than each taking their
 * own: driving the game to a scene is the slow part, and two captures taken
 * minutes apart are two different moments of a running game, which makes their
 * results not quite comparable.
 *
 *   node tools/grade-all.mjs                # capture, then grade everything
 *   node tools/grade-all.mjs --no-capture   # reuse the last capture
 *   node tools/grade-all.mjs --stage 0      # pin a scene first
 *
 * Exits non-zero if any grader did. Note what that does and does not mean: a
 * clean run says the emulator agrees with the explorer on everything that could
 * be measured, and the summary says what could not be.
 */
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { captureBoard, DEFAULT_OUT } from './lib/capture.mjs';
import { parseArgs } from './lib/args.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const args = parseArgs(['stage', 'port', 'out', 'polls', 'every']);
const out = args.str('out', DEFAULT_OUT);
const port = args.num('port', 7172);

/* ---- one capture, shared ------------------------------------------------- */

if (!args.bool('no-capture')) {
    console.log('taking a capture for the scene-dependent graders…');
    const attach = args.bool('attach');
    const emu = attach ? await M2Hle.attach({ port })
                       : await M2Hle.launch({ rom: findRom().primary, port });
    try {
        await emu.waitUntilRunning();
        const m = await captureBoard(emu, {
            out, stage: args.num('stage', null),
            polls: args.num('polls', 20), every: args.num('every', 30),
            log: (s) => console.log('  ' + s),
        });
        console.log(`  scene ${m.scene}, ${m.frames} frames -> ${out}\n`);
    } finally {
        await emu.close({ kill: !attach });
    }
}

/* ---- then each grader ---------------------------------------------------- */

/* Separate processes on purpose. Each grader launches and kills its own
 * emulator where it needs one, and one of them crashing should not take the
 * rest of the run with it. */
const GRADERS = [
    ['grade-models.mjs', []],
    ['grade-texram.mjs', [out]],
    ['grade-colors.mjs', [out]],
];

const results = [];
for (const [script, extra] of GRADERS) {
    const r = spawnSync(process.execPath, [path.join(HERE, script), ...extra], {
        stdio: 'inherit', cwd: HERE,
    });
    results.push({ script, code: r.status ?? -1 });
}

console.log('─'.repeat(72));
for (const r of results) {
    console.log(`  ${r.code === 0 ? 'ok  ' : 'FAIL'}  ${r.script}` +
                (r.code > 0 ? `  (exit ${r.code})` : ''));
}
console.log('─'.repeat(72) + '\n');
process.exitCode = results.some((r) => r.code !== 0) ? 1 : 0;
