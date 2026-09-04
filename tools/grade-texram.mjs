/*
 * grade-texram.mjs — hold the emulator's texture RAM against two other boards.
 *
 * About 85% of the game's texture pages are compressed in ROM, so the sheets do
 * not exist anywhere in a readable form: `unpack_lod_data` builds them into
 * texture RAM on every scene change. That makes texture RAM an unusually good
 * thing to grade on — it is a megabyte of output from a long run of the game's
 * own code, and a single wrong bit anywhere in the i960 core, the bus or the
 * decompressor shows up in it.
 *
 * Three parties, and the grade says which pair it managed to compare:
 *
 *   emulator   what m2-hle2's i960 wrote into texture RAM, captured live
 *   explorer   what noclip's js/texture.js builds from the same ROM
 *   board      SHA-256 over a MAME capture of the real hardware (ref/)
 *
 * Emulator against explorer is the everyday measurement: two independent ports
 * of the same routine, and a disagreement is a bug in one of them. Either
 * against the board digests is the one that settles which. The board comparison
 * only applies to the scene the capture was taken from — South Island with set
 * 16 resident — so it is skipped, loudly, on any other.
 *
 *   node tools/grade-texram.mjs --capture          # take a fresh one and grade
 *   node tools/grade-texram.mjs out/board          # grade one already taken
 */
import { M2Hle } from './lib/m2hle.mjs';
import { loadRom } from './lib/rom.mjs';
import { nc, noclipVersion } from './lib/noclip.mjs';
import { captureBoard, readCapture, DEFAULT_OUT } from './lib/capture.mjs';
import { assertSizes } from './lib/board.mjs';
import { Report, diffBytes, diffDetail } from './lib/report.mjs';
import { loadRef, sheetDigest, badBlocks, BLOCK_BYTES } from './lib/texref.mjs';
import { parseArgs } from './lib/args.mjs';

/* Set 16 goes in first because the attract and character-select screens leave
 * it resident, and the stage sets do not overwrite its deepest mip levels. */
const RESIDENT_SET = 16;

const args = parseArgs(['stage', 'port', 'polls', 'every', 'out']);
const ref = loadRef();
const rep = new Report('grade-texram — emulator vs explorer vs board');

await assertSizes();

/* ---- 1. get a capture ---------------------------------------------------- */

const out = args.str('out', args.positional[0] ?? DEFAULT_OUT);
let capture;

if (args.bool('capture')) {
    /* Default to the scene the board digests were taken from, so the three-way
     * comparison is available rather than skipped. */
    const stage = args.num('stage', ref ? ref.stage : 0);
    const port = args.num('port', 7172);
    const attach = args.bool('attach');
    const emu = attach
        ? await M2Hle.attach({ port })
        : await M2Hle.launch({ rom: (await import('./lib/rom.mjs')).findRom().primary, port });
    try {
        const st = await emu.waitUntilRunning();
        rep.note(`emulator running: profile=${st.profile}, pinning stage ${stage}`);
        await captureBoard(emu, {
            out, stage,
            polls: args.num('polls', 40),
            every: args.num('every', 30),
            log: (s) => rep.note(s),
        });
    } finally {
        await emu.close({ kill: !attach });
    }
    capture = readCapture(out);
} else {
    capture = readCapture(out);
}

const { manifest, bytes } = capture;

/* The scene to build the explorer's answer for is the one read off the record
 * change_scene loaded, not the one stage_num happens to hold and not the one
 * that was asked for. Getting this backwards is how a capture of the wrong
 * arena grades cleanly against the right one's build. */
const scene = manifest.scene;
rep.note(`capture: scene ${scene ?? 'unidentified'} (stage_num=${manifest.stageNum}` +
         `${manifest.stagePinned !== null ? `, pinned ${manifest.stagePinned}` : ''}), ` +
         `${manifest.frames} frames, taken ${manifest.takenAt}`);
if (!manifest.settled) rep.note('WARNING: the upload was still moving when it was taken');
if (manifest.stagePinned !== null && manifest.stageReached === false) {
    rep.note(`WARNING: stage ${manifest.stagePinned} never loaded; this is scene ${scene}`);
}
if (manifest.sceneAmbiguous) {
    rep.note(`note: stages [${manifest.sceneAmbiguous}] share these texture sets`);
}
if (scene === null) {
    rep.skip('everything', 'the loaded stage record matches no record in ROM — ' +
                           'the game is not in an arena');
    rep.finish();
    process.exit();
}

/* ---- 2. build the explorer's answer for the same scene ------------------- */

const { rom, primary } = await loadRom();
const { readStageTable } = await nc('stages.js');
const { buildTexram } = await nc('texture.js');

const stages = readStageTable(rom);
if (!(scene >= 0 && scene < stages.length)) {
    rep.skip('everything', `capture holds scene ${scene}, ` +
                           `outside the table's 0..${stages.length - 1}`);
    rep.finish();
    process.exit();
}
/* Cross-check the explorer's reading of the record against the board's own: the
 * texture words the emulator has loaded must be the ones the explorer read out
 * of the same ROM. If these disagree, nothing below means anything. */
const romWords = stages[scene].texSet;
rep.check('stage record: emulator vs explorer',
          romWords[0] === manifest.sceneTexWords[0] && romWords[1] === manifest.sceneTexWords[1],
          `tex words [${manifest.sceneTexWords.map((v) => '0x' + v.toString(16))}] ` +
          `vs [${romWords.map((v) => '0x' + v.toString(16))}]`);

const sets = [RESIDENT_SET, ...stages[scene].texSets];
const { sheet0, sheet1, pages } = buildTexram(rom, sets);
rep.note(`explorer ${noclipVersion()}: scene ${scene} sets [${sets}] -> ${pages} pages`);
rep.note(`rom: ${primary}`);

const SHEETS = [['texram0', sheet0], ['texram1', sheet1]];

/* ---- 3. emulator vs explorer -------------------------------------------- */

for (const [name, theirs] of SHEETS) {
    const ours = bytes[name];
    if (!ours) { rep.skip(`${name}: emulator vs explorer`, 'not in the capture'); continue; }
    const d = diffBytes(ours, theirs);
    rep.check(`${name}: emulator vs explorer`, d.equal, diffDetail(d));
}

/* ---- 4. and against the board ------------------------------------------- */

if (!ref) {
    rep.skip('board digests', 'ref/texram-ref.json is not here');
} else if (scene !== ref.stage || sets.join() !== ref.sets.join()) {
    /* Not a failure: the digests describe one scene, and this is another. Say
     * which, so it is obvious how to get the comparison back. */
    rep.skip('board digests',
             `capture is scene ${scene} sets [${sets}]; the board capture ` +
             `is stage ${ref.stage} sets [${ref.sets}] — re-run with --capture --stage ${ref.stage}`);
} else {
    for (const [name, theirs] of SHEETS) {
        const ours = bytes[name];
        if (ours) {
            const ok = sheetDigest(ours).whole === ref[name].whole;
            rep.check(`${name}: emulator vs board`, ok,
                      ok ? `${ours.length} bytes byte-exact against the MAME capture`
                         : blockDetail(ours, ref[name]));
        }
        const okNc = sheetDigest(theirs).whole === ref[name].whole;
        rep.check(`${name}: explorer vs board`, okNc,
                  okNc ? 'the anchor still applies to this explorer commit'
                       : blockDetail(theirs, ref[name]));
    }
}

/* ---- 5. luma RAM --------------------------------------------------------- */

const { buildLumaram } = await nc('colors.js');
const luma = buildLumaram(rom);
if (bytes.lumaram) {
    const d = diffBytes(bytes.lumaram, luma);
    rep.check('lumaram: emulator vs explorer', d.equal, diffDetail(d));
} else {
    rep.skip('lumaram: emulator vs explorer', 'not in the capture');
}
if (ref?.lumaram) {
    const { sha } = await import('./lib/texref.mjs');
    if (bytes.lumaram) {
        rep.check('lumaram: emulator vs board', sha(bytes.lumaram) === ref.lumaram);
    }
    rep.check('lumaram: explorer vs board', sha(luma) === ref.lumaram);
}

rep.finish();

function blockDetail(ours, refSheet) {
    const blocks = badBlocks(ours, refSheet);
    return `${blocks.length} of ${refSheet.blocks.length} ${BLOCK_BYTES / 1024}kB blocks differ, ` +
           `first at 0x${(blocks[0] * BLOCK_BYTES).toString(16)}`;
}
