/*
 * texref.mjs — the MAME anchor, and the slices it is cut at.
 *
 * Why this exists at all. Everything else in this toolkit compares the emulator
 * against the explorer: two ports of the same routines, written from the same
 * reverse engineering. When they agree that is worth something, but it is not
 * proof — two ports can share a misreading, and a check written against one of
 * them cannot see it. What breaks that tie is a third point that is neither:
 * `ref/texram-ref.json`, SHA-256 over a MAME capture of the real board's
 * texture RAM, luma RAM and colorxlat.
 *
 * So a texture or colour grade is three-way. Emulator against explorer says the
 * two ports agree; either one against these digests says the agreement is with
 * the machine. A grade that can only make the first statement says so.
 *
 * The manifest is a few kilobytes of hashes rather than 2.2 MB of the game's
 * data, and makes the same assertion: a single wrong texel still fails. It
 * cannot be turned back into the capture.
 *
 * Provenance. Both the manifest and the hashing rules below come from the
 * explorer's own toolkit (stf-tools `texram-ref.json` / `texref.mjs`), where
 * they were built by `make-texref.mjs` from a MAME capture taken with South
 * Island on screen. They are copied rather than derived: a manifest rebuilt
 * from either port would make every check that reads it a tautology, which is
 * exactly the trap this file exists to avoid. If stf-tools ever regenerates the
 * manifest, copy the new one over — do not regenerate it here.
 *
 * The slicing has to match the manifest's exactly, so these functions are kept
 * byte-identical to the ones that wrote it.
 */
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { TOOLS } from './noclip.mjs';

export const REF_PATH = path.join(TOOLS, 'ref', 'texram-ref.json');

/** The manifest, or null if it is not here — a grader should skip, not fail. */
export function loadRef() {
    if (!fs.existsSync(REF_PATH)) return null;
    return JSON.parse(fs.readFileSync(REF_PATH, 'utf8'));
}

/* A sheet is hashed whole and again in 64 kB blocks. The whole digest is the
 * check; the blocks only say where a failure is, which is the one thing a
 * digest otherwise takes away. */
export const BLOCK_BYTES = 0x10000;
const BLOCK_HEX = 16;

const CXLAT_CHANNEL = 0x4000;
const CXLAT_CHANNELS = 3;
const CXLAT_LUMA = 256;

export const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');

export function sheetDigest(bytes) {
    const blocks = [];
    for (let o = 0; o < bytes.length; o += BLOCK_BYTES) {
        blocks.push(sha(bytes.subarray(o, o + BLOCK_BYTES)).slice(0, BLOCK_HEX));
    }
    return { whole: sha(bytes), blocks };
}

/** Which 64 kB blocks of a sheet disagree with the manifest's, for reporting. */
export function badBlocks(bytes, ref) {
    const ours = sheetDigest(bytes).blocks;
    const out = [];
    for (let i = 0; i < ours.length; i++) if (ours[i] !== ref.blocks[i]) out.push(i);
    return out;
}

/* colorxlat is three channels of 32 rows of 256 sixteen-bit entries, and the
 * colour check compares it row by row rather than whole: some rows are a
 * character's, some are the stage's, and two are rotating while the capture is
 * taken. So a row is hashed across all three channels at once — that is the
 * unit every one of those comparisons is made in. */
export function cxlatRowBytes(a, row, luma0 = 0, luma1 = CXLAT_LUMA, skip = null) {
    const out = [];
    for (let ch = 0; ch < CXLAT_CHANNELS; ch++) {
        for (let luma = luma0; luma < luma1; luma++) {
            if (skip && luma >= skip[0] && luma < skip[1]) continue;
            const o = ch * CXLAT_CHANNEL + (row * CXLAT_LUMA + luma) * 2;
            out.push(a[o], a[o + 1]);
        }
    }
    return Uint8Array.from(out);
}

export const cxlatRowDigest = (a, row, luma0, luma1, skip) =>
    sha(cxlatRowBytes(a, row, luma0, luma1, skip));
