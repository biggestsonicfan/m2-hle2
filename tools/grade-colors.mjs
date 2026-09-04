/*
 * grade-colors.mjs — hold the emulator's colour tables against the explorer's.
 *
 * colorxlat is what actually decides what the screen looks like. A face's shade
 * is a luma band into one of 32 palette rows, and those rows are written by four
 * different routines at four different times: a ramp at boot, a flat band on
 * every scene request, the stage's own sixteen colours when a scene loads, and a
 * fighter's part and skin blocks when one is selected. A table that is right in
 * three of those and wrong in the fourth renders a scene that looks plausible
 * and is wrong, which is exactly the failure this measures.
 *
 * The rows are compared in groups because they are written in groups, and the
 * two rows the game rotates every few frames are handled apart from the rest:
 *
 *   the ramp, the flat band, the stage rows   compared outright
 *   the fighter rows                          compared by asking who is on screen
 *   the cycled rows (the sea, the waterfall)  compared at every rotation
 *
 * The cycled pair is the interesting one. sub_2435C rotates two sixteen-colour
 * bands as the frame counter advances, so a capture catches them part way round
 * and a plain comparison fails for the right reason at the wrong moment. Instead
 * each has to match at exactly one rotation, and one value of frame_counter has
 * to explain both at once — a stronger statement than "some rotation matches",
 * because it says the two implementations agree about which colour is in which
 * slot at which moment.
 *
 *   node tools/grade-colors.mjs --capture
 *   node tools/grade-colors.mjs out/board
 */
import path from 'node:path';
import { M2Hle } from './lib/m2hle.mjs';
import { loadRom, findRom } from './lib/rom.mjs';
import { nc, noclipVersion } from './lib/noclip.mjs';
import { captureBoard, readCapture, DEFAULT_OUT } from './lib/capture.mjs';
import { assertSizes } from './lib/board.mjs';
import { Report } from './lib/report.mjs';
import { loadRef, cxlatRowDigest } from './lib/texref.mjs';
import { parseArgs } from './lib/args.mjs';

const CXLAT_ROWS = 32;
const CXLAT_LUMA = 256;
/* A fighter's own rows stop at luma 64; above that is the flat band, which is
 * the stage's and not theirs. */
const FIGHTER_LUMA = 64;
/* Rows 0..4 are player 1's part colours (send_tex_col_part) and 28..29 their
 * skin (send_tex_col_skin). Player 2's — 7..11 and 30..31 — are covered by the
 * outright comparison, since they are zero on both sides unless a second player
 * has joined. */
const FIGHTER_ROWS = [0, 1, 2, 3, 4, 28, 29];
/* The twelve fighters share one part block and one skin block, so all twelve
 * reproduce each other's rows. Getting exactly those twelve back is a statement
 * about the tables; getting one back is a coincidence. */
const FIGHTER_CHARS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
const CHAR_COUNT = 52;

const args = parseArgs(['stage', 'port', 'polls', 'every', 'out']);
const ref = loadRef();
const rep = new Report('grade-colors — colour tables, emulator vs explorer vs board');

await assertSizes();

/* ---- 1. a capture -------------------------------------------------------- */

const out = args.str('out', args.positional[0] ?? DEFAULT_OUT);
if (args.bool('capture')) {
    const port = args.num('port', 7172);
    const attach = args.bool('attach');
    const emu = attach ? await M2Hle.attach({ port })
                       : await M2Hle.launch({ rom: findRom().primary, port });
    try {
        await emu.waitUntilRunning();
        await captureBoard(emu, {
            out, stage: args.num('stage', null),
            polls: args.num('polls', 20), every: args.num('every', 30),
            log: (s) => rep.note(s),
        });
    } finally {
        await emu.close({ kill: !attach });
    }
}
const { manifest, bytes } = readCapture(out);
const ours = bytes.colorxlat;
if (!ours) {
    rep.skip('everything', 'the capture has no colorxlat.bin');
    rep.finish();
    process.exit();
}

const scene = manifest.scene;
rep.note(`capture: scene ${scene ?? 'unidentified'}, ${manifest.frames} frames`);
if (scene === null) {
    rep.skip('everything', 'the loaded stage record matches no record in ROM');
    rep.finish();
    process.exit();
}

/* ---- 2. the explorer's tables for the same scene ------------------------- */

const { rom, primary } = await loadRom();
const { readStageTable } = await nc('stages.js');
const { buildColorxlat, cycleStageColors, PALETTE_LUMA0 } = await nc('colors.js');
const stage = readStageTable(rom)[scene];
rep.note(`explorer ${noclipVersion()}, rom ${path.basename(primary)}, ` +
         `colorSet ${stage.texSet[1]}, ${stage.colorCycles.length} cycled rows`);

const opts = { colorSet: stage.texSet[1], tint: stage.tint };
const cycledRows = stage.colorCycles.map((c) => c.row);

/* A cycle record names a row and a shift, not a band — the band it rotates is
 * always the sixteen palette slots, from where the palette starts up to where
 * the flat band begins. Taken from the explorer's own constant rather than
 * written down again here, and cross-checked against the board capture's when
 * that is available, since the two describing different slots would make every
 * comparison below meaningless. */
const cycleBand = [PALETTE_LUMA0, FIGHTER_LUMA];
if (ref?.cycleBand && ref.cycleBand.join() !== cycleBand.join()) {
    rep.check('cycled band', false,
              `the explorer rotates luma [${cycleBand}] but the board capture ` +
              `was cut at [${ref.cycleBand}] — one of them is wrong`);
}

/* ---- 3. who is on screen ------------------------------------------------- */

/* The capture cannot say, so ask which characters could have been: the ones
 * whose part and skin blocks reproduce the fighter rows the emulator holds. */
const candidates = [];
for (let c = 0; c < CHAR_COUNT; c++) {
    const t = buildColorxlat(rom, { ...opts, fighters: [c] });
    if (FIGHTER_ROWS.every((row) =>
        cxlatRowDigest(t, row, 0, FIGHTER_LUMA, null) ===
        cxlatRowDigest(ours, row, 0, FIGHTER_LUMA, null))) candidates.push(c);
}

const noFighter = buildColorxlat(rom, { ...opts, fighters: [] });
const emptyFighter = FIGHTER_ROWS.every((row) =>
    cxlatRowDigest(noFighter, row, 0, FIGHTER_LUMA, null) ===
    cxlatRowDigest(ours, row, 0, FIGHTER_LUMA, null));

if (emptyFighter) {
    rep.note('no fighter rows in this capture — nobody was on screen');
} else if (candidates.join() === FIGHTER_CHARS.join()) {
    rep.check('fighter rows: emulator vs explorer', true,
              `${FIGHTER_ROWS.length} rows exact for the twelve fighters ` +
              `(${candidates[0]}..${candidates[candidates.length - 1]}), which share one block`);
} else if (candidates.length) {
    rep.check('fighter rows: emulator vs explorer', true,
              `reproduced by characters [${candidates.join(', ')}]`);
} else {
    rep.check('fighter rows: emulator vs explorer', false,
              'no character in the table reproduces the rows the emulator holds');
}

/* Any candidate gives the same bytes for those rows; with none, build with no
 * fighter so the comparison below is about the rest of the table. */
const theirs = buildColorxlat(rom, {
    ...opts, fighters: candidates.length ? [candidates[0]] : [],
});

/* ---- 4. the rest of the table, row by row -------------------------------- */

const bad = [];
for (let row = 0; row < CXLAT_ROWS; row++) {
    /* A cycled row's rotating band is checked below instead. */
    const skip = cycledRows.includes(row) ? cycleBand : null;
    if (cxlatRowDigest(ours, row, 0, CXLAT_LUMA, skip) ===
        cxlatRowDigest(theirs, row, 0, CXLAT_LUMA, skip)) continue;
    bad.push(row);
}
rep.check('colorxlat: emulator vs explorer', bad.length === 0,
          bad.length ? `rows [${bad.join(', ')}] differ` +
                       (cycledRows.length ? ` (rows [${cycledRows}] excluding their cycled band)` : '')
                     : `all ${CXLAT_ROWS} rows exact` +
                       (cycledRows.length ? `, bar the cycled band on rows [${cycledRows}]` : ''));

/* ---- 5. the cycled rows, at every rotation ------------------------------- */

if (!stage.colorCycles.length) {
    rep.skip('cycled rows', `scene ${scene} rotates no colours`);
} else {
    const found = [];
    let ambiguous = 0;
    for (const c of stage.colorCycles) {
        const hits = [];
        for (let phase = 0; phase < 16; phase++) {
            const t = Uint8Array.from(theirs);
            cycleStageColors(rom, t, { ...opts, row: c.row, phase });
            if (cxlatRowDigest(t, c.row, cycleBand[0], cycleBand[1], null) ===
                cxlatRowDigest(ours, c.row, cycleBand[0], cycleBand[1], null)) hits.push(phase);
        }
        if (hits.length !== 1) ambiguous++;
        found.push({ ...c, phase: hits.length === 1 ? hits[0] : -1, hits: hits.length });
    }
    rep.check('cycled rows match at exactly one rotation each', ambiguous === 0,
              found.map((c) => `row ${c.row}: ` +
                  (c.hits === 0 ? 'no rotation matches'
                   : c.hits === 1 ? `phase ${c.phase}` : `${c.hits} rotations match`)).join(', '));

    /* One frame_counter has to explain every row at once. The list repeats every
     * 16 << max(shift) frames, so that whole period is searched. */
    if (!ambiguous) {
        const period = 16 << Math.max(0, ...found.map((c) => c.shift));
        const frames = [];
        for (let f = 0; f < period; f++) {
            if (found.every((c) => ((f >>> c.shift) & 15) === c.phase)) frames.push(f);
        }
        rep.check('one frame_counter explains every rotation', frames.length > 0,
                  frames.length ? `frame_counter ${frames[0]} mod ${period}`
                                : 'no single frame counter explains the captured rotations');
    }
}

/* ---- 6. and against the board ------------------------------------------- */

if (!ref) {
    rep.skip('board digests', 'ref/texram-ref.json is not here');
} else if (scene !== ref.stage) {
    rep.skip('board digests',
             `capture is scene ${scene}; the board capture is stage ${ref.stage} — ` +
             `re-run with --capture --stage ${ref.stage}`);
} else {
    const boardBad = [];
    for (let row = 0; row < CXLAT_ROWS; row++) {
        const skip = ref.cycledRows.includes(row) ? ref.cycleBand : null;
        if (cxlatRowDigest(ours, row, 0, CXLAT_LUMA, skip) === ref.colorxlat.rows[row]) continue;
        boardBad.push(row);
    }
    rep.check('colorxlat: emulator vs board', boardBad.length === 0,
              boardBad.length ? `rows [${boardBad.join(', ')}] differ from the MAME capture`
                              : 'every row byte-exact against the MAME capture');
}

rep.finish();
