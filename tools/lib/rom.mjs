/*
 * rom.mjs — find the ROM set, and decode it the way the explorer does.
 *
 * Nothing here carries a ROM and nothing here will accept one into the tree:
 * the repository's .gitignore refuses *.zip outright. A grader is handed the
 * user's own dump, and both sides of every comparison read that same file — the
 * emulator loads it, and the explorer's romset.js assembles it — so a grade can
 * never be measuring two different games.
 *
 * Search order, first hit wins:
 *
 *   $STF_ROM             an explicit path to the zip
 *   $ROMS_DIR            the machine's one ROM folder (ROMS_DIR below)
 *   <repo>/sfight.zip    a dump dropped in the checkout (gitignored)
 *   <repo>/roms/         likewise (gitignored)
 *   ../stf-tools, ../noclip   a sibling checkout that already has one
 *
 * A split set needs schamp.zip beside sfight.zip: the program EPROMs come from
 * the clone and the mask ROMs from the parent. A non-merged sfight.zip carries
 * everything and is enough on its own.
 *
 * Use a set where it lies; never copy one into a scratch directory. M2Hle.launch
 * runs the emulator from a directory of its own, so nothing is written beside
 * the ROM and no loose sfight/epr-* file there is taken for the program.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { REPO, nc } from './noclip.mjs';

const NAMES = ['sfight.zip', 'schamp.zip'];

/* The dev container keeps every stock set it has in one folder of symlinks
 * (antigravity-dev-docker's link-roms.sh): if a set is not there, it is not in
 * the container. $ROMS_DIR names it; elsewhere the default is simply absent. */
export const ROMS_DIR = process.env.ROMS_DIR || path.join(os.homedir(), 'build', 'mameroms');

/** MAME's -rompath: $MAME_ROMPATH, else ROMS_DIR (zips only, no loose sfight/
 *  folder for MAME to prefer), else tools/mame/mameroms. */
export function mameRomPath() {
    if (process.env.MAME_ROMPATH) return process.env.MAME_ROMPATH;
    if (fs.existsSync(path.join(ROMS_DIR, 'sfight.zip'))) return ROMS_DIR;
    return path.join(REPO, 'tools', 'mame', 'mameroms');
}

/** Directories a ROM set might be sitting in, in preference order. */
export function romSearchPath() {
    const dirs = [];
    if (process.env.STF_ROM) dirs.push(path.dirname(path.resolve(process.env.STF_ROM)));
    dirs.push(ROMS_DIR, REPO, path.join(REPO, 'roms'));
    dirs.push(path.resolve(REPO, '..', 'stf-tools'), path.resolve(REPO, '..', 'noclip'));
    return dirs;
}

/**
 * Locate the ROM set.
 * @returns {{primary: string, zips: string[], dir: string}}
 *   `primary` is what the emulator is pointed at; `zips` is everything the
 *   explorer's loader should be handed, which is both halves of a split set.
 */
export function findRom() {
    if (process.env.STF_ROM && fs.existsSync(process.env.STF_ROM)) {
        const primary = path.resolve(process.env.STF_ROM);
        const dir = path.dirname(primary);
        const zips = [primary];
        for (const n of NAMES) {
            const p = path.join(dir, n);
            if (p !== primary && fs.existsSync(p)) zips.push(p);
        }
        return { primary, zips, dir };
    }
    for (const dir of romSearchPath()) {
        const here = NAMES.map((n) => path.join(dir, n)).filter((p) => fs.existsSync(p));
        if (!here.length) continue;
        /* sfight.zip is the clone and carries the program EPROMs, so it is what
         * the emulator is pointed at whenever both are present. */
        const primary = here.find((p) => p.endsWith('sfight.zip')) ?? here[0];
        return { primary, zips: here, dir };
    }
    throw new Error(
        'no ROM set found — looked in:\n  ' + romSearchPath().join('\n  ') +
        '\n\nDrop sfight.zip (and schamp.zip for a split set) in the repository ' +
        'root, or point $STF_ROM at one. None is carried here.');
}

const readAB = (p) => {
    const b = fs.readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

/**
 * Assemble the ROM regions through the explorer's own loader — the same
 * interleave the emulator's sfight profile does, and the same one MAME's
 * ROM_LOAD32_WORD describes. Returns { rom, zips, primary }.
 */
export async function loadRom(where = null) {
    const found = where ?? findRom();
    const { loadRomSet } = await nc('romset.js');
    const rom = await loadRomSet(found.zips.map(readAB));
    return { rom, ...found };
}
