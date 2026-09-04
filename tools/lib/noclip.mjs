/*
 * noclip.mjs — locate the explorer whose decoders are this toolkit's oracle.
 *
 * The tools here grade the emulator against the Sonic The Fighters explorer:
 * a second, independent implementation of the same ROM formats, held to a real
 * machine by its own checks. That makes it worth importing rather than copying,
 * so it is a submodule at vendor/noclip and every tool reads it from there.
 *
 * Resolution order, first hit wins:
 *
 *   $M2_NOCLIP           an explorer checkout you are working on
 *   vendor/noclip        the pinned submodule — the normal case
 *   ../noclip            a sibling checkout, for a tree cloned without --recursive
 *
 * The submodule is pinned to a commit on purpose: a grade is always measured
 * against a known explorer rather than against whatever master happens to be.
 * To move it forward, `git -C vendor/noclip pull`, re-run the graders, and
 * commit the new pointer.
 *
 * The explorer's modules are browser ES modules, but nothing in the ones this
 * toolkit imports touches the DOM — zip.js goes through DecompressionStream and
 * Blob, which Node has had since 18 — so they load unchanged. That is the whole
 * reason this is a Node toolkit rather than a Python one.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const TOOLS = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
export const REPO = path.dirname(TOOLS);

function resolveNoclip() {
    const tried = [];
    for (const base of [process.env.M2_NOCLIP,
                        path.join(REPO, 'vendor', 'noclip'),
                        path.resolve(REPO, '..', 'noclip')]) {
        if (!base) continue;
        tried.push(base);
        if (fs.existsSync(path.join(base, 'js', 'romset.js'))) return base;
    }
    throw new Error(
        'no noclip checkout found — looked in:\n  ' + tried.join('\n  ') +
        '\n\nThe explorer is a submodule. Fetch it with:\n' +
        '  git submodule update --init vendor/noclip\n' +
        'or point $M2_NOCLIP at a checkout.');
}

export const NOCLIP = resolveNoclip();

/** Import one of the explorer's modules by file name, e.g. `nc('model.js')`. */
export const nc = (name) => import(pathToFileURL(path.join(NOCLIP, 'js', name)).href);

/** The commit the grade was measured against, for a report to name. */
export function noclipVersion() {
    try {
        const head = fs.readFileSync(path.join(NOCLIP, '.git'), 'utf8').trim();
        const dir = head.startsWith('gitdir:')
            ? path.resolve(NOCLIP, head.slice(7).trim())
            : path.join(NOCLIP, '.git');
        const ref = fs.readFileSync(path.join(dir, 'HEAD'), 'utf8').trim();
        if (!ref.startsWith('ref:')) return ref.slice(0, 12);
        return fs.readFileSync(path.join(dir, ref.slice(4).trim()), 'utf8').trim().slice(0, 12);
    } catch { return 'unknown'; }
}
