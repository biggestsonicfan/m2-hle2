/*
 * capture.mjs — take a scene's board state out of a running emulator.
 *
 * The m2-hle2 half of what MAME's mame-dump-texram.lua does: run the game until
 * it has a scene loaded, then copy texture RAM, palette RAM, luma RAM and the
 * colour translation table out whole.
 *
 * Getting this wrong is easy and quiet, so it is worth stating what the three
 * stages below are actually for.
 *
 * 1. Pinning. stage_num is a byte the front end sets and change_scene reads, so
 *    holding it every frame is how the game is walked into a chosen arena —
 *    the same trick the MAME capture Lua uses, and for the same reason: writing
 *    it once is not enough, because the front end writes it back.
 *
 * 2. Verifying. Pinning is not arriving. stage_num only sets what the draw
 *    routines branch on; the scene was chosen the last time change_scene ran,
 *    which may have been long before the pin. So the capture waits for the
 *    64-word record change_scene copies to 0x504800 to be the record the ROM
 *    holds for the stage that was asked for — on its texture-set words, which
 *    are the part of it the running game does not go on rewriting.
 *
 *    Without this the tool will happily dump whatever set was already resident
 *    and label it with the stage that was pinned. That is not a capture that
 *    merely fails; it is one that grades cleanly against the wrong scene.
 *
 * 3. Settling. The game unpacks a scene's pages over several frames, so a dump
 *    taken the moment a record lands catches a half-filled sheet. Settling is
 *    measured on the sheet's digest rather than on how much of it is non-zero —
 *    a count that has stopped moving is not the same as content that has, and
 *    two different scenes can fill exactly as many bytes as each other.
 *
 * What it writes is the game's own data. It defaults to a temp directory
 * outside the checkout for that reason, and nothing that grades the emulator
 * may be built from it: it is the port's own output, and a reference built from
 * it would make every check that reads it a tautology.
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { createHash } from 'node:crypto';
import {
    REGIONS, STAGE_NUM, STAGE_DATA, STAGE_STRIDE, STAGE_LOADED, STAGE_TEX_A, STAGE_TEX_B,
} from './board.mjs';
import { noclipVersion } from './noclip.mjs';

export const DEFAULT_OUT = path.join(os.tmpdir(), 'm2hle-board');

/** A sheet holding less than this is one nothing has been uploaded into yet. */
const MIN_NONZERO = 0x10000;

const u16 = (b, o) => b[o] | (b[o + 1] << 8);

/* ---- which scene is actually loaded -------------------------------------- */

/** The two texture-set words of the record change_scene copied. */
async function loadedTexWords(emu) {
    const b = await emu.readMemory(STAGE_LOADED + STAGE_TEX_A, 4);
    return [u16(b, 0), u16(b, 2)];
}

/** The same two words from stage `n`'s record in the program ROM. */
async function romTexWords(emu, n) {
    const b = await emu.readMemory(STAGE_DATA + n * STAGE_STRIDE + STAGE_TEX_A, 4);
    return [u16(b, 0), u16(b, 2)];
}

/**
 * Which stage record the game currently has loaded, read off the board rather
 * than inferred from stage_num.
 *
 * Returns { stage, texWords, ambiguous } — `stage` is null if no record
 * matches, and `ambiguous` lists every stage that matches when more than one
 * does, since several arenas can share a texture pair.
 */
export async function identifyScene(emu, stageCount = 16) {
    const texWords = await loadedTexWords(emu);
    const hits = [];
    for (let n = 0; n < stageCount; n++) {
        const w = await romTexWords(emu, n);
        if (w[0] === texWords[0] && w[1] === texWords[1]) hits.push(n);
    }
    return { stage: hits.length ? hits[0] : null, texWords, ambiguous: hits.length > 1 ? hits : null };
}

/* ---- driving ------------------------------------------------------------- */

/**
 * Hold stage_num at `stage` until the game has actually loaded that record.
 *
 * In attract mode the game changes scene on its own every half minute or so and
 * reads the pinned stage_num when it does, so this is mostly a matter of
 * holding the byte and waiting. Returns { arrived, polls, stage }.
 */
export async function reachStage(emu, stage, { polls = 90, every = 30, log = () => {} } = {}) {
    const want = await romTexWords(emu, stage);
    for (let p = 0; p < polls; p++) {
        for (let f = 0; f < every; f++) {
            await emu.writeMemory(STAGE_NUM, [stage & 0xff]);
            const w = await emu.waitFrames(1, 8000);
            if (!w.reached) return { arrived: false, polls: p, stage: null, stalled: true };
        }
        const have = await loadedTexWords(emu);
        if (have[0] === want[0] && have[1] === want[1]) {
            log(`stage ${stage} loaded after ${(p + 1) * every} frames ` +
                `(tex words ${have.map((v) => '0x' + v.toString(16)).join(', ')})`);
            return { arrived: true, polls: p + 1, stage };
        }
        log(`waiting for stage ${stage}: loaded record has tex words ` +
            `${have.map((v) => '0x' + v.toString(16)).join(', ')}, want ` +
            `${want.map((v) => '0x' + v.toString(16)).join(', ')}`);
    }
    return { arrived: false, polls, stage: null };
}

/* ---- capturing ----------------------------------------------------------- */

/**
 * Run until texture RAM stops changing, then dump the regions.
 *
 * @param {import('./m2hle.mjs').M2Hle} emu   an emulator already running
 * @param {object} o
 * @param {string} [o.out]      directory to write into
 * @param {?number} [o.stage]   reach and hold this scene; null takes what is up
 * @param {number} [o.polls]    settle polls before giving up
 * @param {number} [o.every]    frames between polls
 * @param {number} [o.reachPolls] polls to spend waiting for the scene to load
 * @returns {Promise<object>} the manifest that was written
 */
export async function captureBoard(emu, {
    out = DEFAULT_OUT, stage = null, polls = 20, every = 30, reachPolls = 90,
    log = () => {},
} = {}) {
    const scratch = path.join(os.tmpdir(), `m2hle-settle-${process.pid}.bin`);
    try {
        let reached = null;
        if (stage !== null) {
            reached = await reachStage(emu, stage, { polls: reachPolls, every, log });
            if (!reached.arrived) {
                log(`WARNING: stage ${stage} never loaded — capturing whatever is up`);
            }
        }

        /* Settle on the sheet's digest. A stable non-zero count is not the same
         * statement: two scenes can fill the same number of bytes. */
        let lastDigest = null, stable = 0, used = 0, nonzero = 0;
        for (; used < polls; used++) {
            if (stage !== null) {
                if (!await holdFrames(emu, stage, every)) break;
            } else if (!(await emu.waitFrames(every, 30000)).reached) break;

            const r = await emu.rpc('dump_memory_file', {
                addr: '0x' + REGIONS.texram0.base.toString(16),
                size: REGIONS.texram0.size, path: scratch,
            });
            nonzero = r.nonzero;
            const digest = createHash('sha256').update(fs.readFileSync(scratch)).digest('hex');
            log(`poll ${used + 1}: texram0 ${(nonzero / REGIONS.texram0.size * 100).toFixed(1)}% ` +
                `filled, ${digest.slice(0, 12)}`);

            if (digest === lastDigest && nonzero > MIN_NONZERO) { if (++stable >= 2) break; }
            else stable = 0;
            lastDigest = digest;
        }

        if (nonzero <= MIN_NONZERO) {
            throw new Error('texture RAM never filled — the game did not reach a scene.');
        }

        const scene = await identifyScene(emu);
        const stageNum = (await emu.readMemory(STAGE_NUM, 1))[0];

        fs.mkdirSync(out, { recursive: true });
        const regions = {};
        for (const [name, { base, size }] of Object.entries(REGIONS)) {
            const d = await emu.dumpRegion(base, size, path.join(out, `${name}.bin`));
            regions[name] = { bytes: size, nonzero: d.nonzero };
            log(`${name}.bin  ${size} bytes, ${(d.nonzero / size * 100).toFixed(1)}% non-zero`);
        }

        const final = await emu.status();
        const manifest = {
            source: 'm2-hle2 MCP bridge (tools/lib/capture.mjs)',
            note: 'A capture of this emulator, not of hardware. It is what the port ' +
                  'produced, so nothing that grades the port may be built from it.',
            takenAt: new Date().toISOString(),

            /* The scene read off the loaded record — this is the one a grader
             * should build the explorer's answer for. stageNum is only what the
             * draw routines are branching on, and the two can disagree. */
            scene: scene.stage,
            sceneTexWords: scene.texWords,
            sceneAmbiguous: scene.ambiguous,
            stageNum,
            stagePinned: stage,
            stageReached: reached ? reached.arrived : null,

            settled: stable >= 2,
            frames: final.frames,
            profile: final.profile,
            noclip: noclipVersion(),
            regions,
        };
        fs.writeFileSync(path.join(out, 'capture.json'),
                         JSON.stringify(manifest, null, 2) + '\n');
        return manifest;
    } finally {
        fs.rmSync(scratch, { force: true });
    }
}

async function holdFrames(emu, stage, frames) {
    for (let f = 0; f < frames; f++) {
        await emu.writeMemory(STAGE_NUM, [stage & 0xff]);
        if (!(await emu.waitFrames(1, 8000)).reached) return false;
    }
    return true;
}

/** Read a capture directory back. Throws with a useful message if it is not one. */
export function readCapture(dir) {
    const manifestPath = path.join(dir, 'capture.json');
    if (!fs.existsSync(manifestPath)) {
        throw new Error(`${dir} is not a capture — no capture.json. ` +
                        'Take one with `node tools/dump-board.mjs <dir>`.');
    }
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    const bytes = {};
    for (const name of Object.keys(REGIONS)) {
        const p = path.join(dir, `${name}.bin`);
        if (fs.existsSync(p)) bytes[name] = new Uint8Array(fs.readFileSync(p));
    }
    return { dir, manifest, bytes };
}
