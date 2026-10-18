#!/usr/bin/env node
/*
 * dc_mdlgroups.mjs -- the model groups the Dreamcast's model pack is laid out by
 * (dreamcast/sfight.mdlgroups, tools/dc_mdlpack.py; Pinboard #489).
 *
 *   node tools/dc_mdlgroups.mjs [--gems <dir of OBJ_*.bin>] > dreamcast/sfight.mdlgroups
 *
 * A group is one scene's objects or one fighter's: a stage, a character, the
 * select screen. Sonic Gems Collection's PS2 build ships its models that way,
 * an OBJ_* file per stage and per character, and the pack does the same with
 * the ROM's bytes, so a fight reads its stage and its two fighters from three
 * places in the pack, whoever fights where.
 *
 * Which objects belong together comes from the explorer (vendor/noclip), which
 * reads it out of the program ROM: every model a stage's display list draws,
 * its animated ones at every frame (display.js), and every part, head and eye
 * model of a fighter (characters.js). With --gems, the decompressed OBJ_*
 * files of the player's own copy of Gems add what the explorer does not
 * reach (the fighters' effects and props, the select screen, the story
 * scenes): a fighter's file joins that fighter, a stage's the stage it
 * overlaps most, and the rest stand as groups of their own. Each object goes
 * in the first group that names it, in the order written.
 *
 * The output is object numbers, which say nothing of the ROM's contents: the
 * pack itself is built from the player's ROM files.
 */
import fs from 'node:fs';
import path from 'node:path';
import { loadRom } from './lib/rom.mjs';
import { nc } from './lib/noclip.mjs';

const MODEL_COUNT = 5103;
const MIRROR = 26;            /* characters.js: the roster's second-player half */

const args = process.argv.slice(2);
const gemsDir = args.includes('--gems') ? args[args.indexOf('--gems') + 1] : null;

const { rom } = await loadRom();
const D = await nc('display.js');
const S = await nc('stages.js');
const C = await nc('characters.js');

const slug = (s) => s.toLowerCase().replace(/['()]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const ok = (m) => m > 0 && m < MODEL_COUNT;

/* The explorer's groups, keyed by name; `from` says where each came from. */
const groups = new Map();
function group(name, from) {
    if (!groups.has(name)) groups.set(name, { name, from: [from], set: new Set() });
    return groups.get(name);
}

/* Stages: what each slot's display list draws, a group per stage number, so
 * the slots that are one stage (South Island's three, Mushroom Hill's two, the
 * Death Egg's three) are one group, named after its first named slot. */
const frames = D.readFrameTables(rom);
const stageGroup = new Map();        /* stage number -> group */
for (const st of S.readStageTable(rom)) {
    let list;
    try { list = D.buildStageDisplayList(st, frames); } catch { continue; }
    if (!stageGroup.has(st.num)) {
        const named = S.readStageTable(rom).find((t) => t.num === st.num && t.named && !/^Stage /.test(t.name));
        const name = st.num === 8 ? 'Death Egg' : (named ?? st).name.replace(/ \(.*\)$/, '');
        stageGroup.set(st.num, group('stage-' + slug(name), `stage slots`));
    }
    const g = stageGroup.get(st.num);
    g.from[0] += ` ${st.slot}`;
    for (const e of list) {
        if (e.anim) for (let f = 0; f < 4096; f++) { const m = D.frameModel(e.anim, f); if (ok(m)) g.set.add(m); }
        else if (ok(e.model)) g.set.add(e.model);
    }
}

/* Fighters: the part tables, normal and squished, and the faces. */
const owners = C.faceVariantOwners(rom);
for (const c of C.CHARACTERS) {
    const r = C.readCharacter(rom, c.index);
    if (!r) continue;
    const g = group('char-' + slug(c.name), `character ${c.index}`);
    for (const m of [...r.partsNormal, ...r.partsSquished, ...r.face.heads, ...r.face.eyes]) if (ok(m)) g.set.add(m);
    for (const [m, owner] of owners) if (owner === c.index) g.set.add(m);
}
for (const g of groups.values()) if (!g.set.size) groups.delete(g.name);

/* Gems' files: base name without the R/V variant suffix, objects in file order. */
const gems = new Map();
if (gemsDir) {
    for (const f of fs.readdirSync(gemsDir).filter((f) => /^OBJ_.*\.bin$/i.test(f)).sort()) {
        const d = fs.readFileSync(path.join(gemsDir, f));
        const n = d.readUInt32LE(0);
        let p = 4;
        const objs = [];
        for (let i = 0; i < n; i++) {
            objs.push(d.readUInt32LE(p));
            p += 4;
            for (let k = 0; k < 3; k++) p += 4 + d.readUInt32LE(p);
        }
        let base = f.slice(4, -4).toUpperCase();
        if (/^(STAGE\d+|[A-Z]{3}\d|NAME|SELECT|ADV|DEMO\d|ENDING\d|BOSS)[RV]$/.test(base)) base = base.slice(0, -1);
        if (!gems.has(base)) gems.set(base, []);
        gems.get(base).push(...objs.filter(ok));
    }
}

/* Gems names a fighter's file by a code and the player side (1, or 2 for the
 * second player's colours, the roster's mirror half), and a stage's by its
 * place in the stage table; a stage file joins the stage it shares most
 * objects with. The rest (the select screen, the story, the endings) stand as
 * groups of their own. */
const FIGHTERS = { SNC: 0, TEI: 1, AMY: 2, MTS: 3, FNG: 4, KMA: 5, KCS: 6, ESP: 7, BIN: 10 };
const charGroup = (index) => [...groups.values()].find((g) => g.from[0] === `character ${index}`);
const out = [...groups.values()];
for (const [base, objs] of gems) {
    let to = null;
    const f = /^([A-Z]{3})([12])$/.exec(base);
    if (f && f[1] in FIGHTERS) to = charGroup(FIGHTERS[f[1]] + (f[2] === '2' ? MIRROR : 0));
    else if (base === 'BOSS') to = charGroup(11);
    else if (/^STAGE\d+$/.test(base)) {
        const set = new Set(objs);
        let bestN = 0;
        for (const g of stageGroup.values()) {
            const n = [...g.set].filter((m) => set.has(m)).length;
            if (n > bestN) { to = g; bestN = n; }
        }
    }
    if (to) {
        to.from.push(`Gems OBJ_${base}`);
        for (const m of objs) to.set.add(m);
    } else {
        out.push({ name: base.toLowerCase(), from: [`Gems OBJ_${base}`], set: new Set(objs) });
    }
}

/* Shared objects first, then the screens before a fight, the stages, the
 * fighters and the story scenes. */
const rank = (g) => g.name === 'common' ? 0 : g.name === 'select' || g.name === 'name' ? 1
    : g.name.startsWith('stage-') ? 2 : g.name.startsWith('char-') ? 3 : 4;
out.sort((a, b) => rank(a) - rank(b));

const seen = new Set();
const lines = [
    '# The model groups of tools/dc_mdlpack.py: `group NAME`, then its objects (model',
    '# table numbers, hex), each in the first group that names it. Written by',
    `# tools/dc_mdlgroups.mjs${gemsDir ? ' --gems' : ''} from the explorer's stages and fighters` +
        (gemsDir ? ' and Sonic' : '.'),
    ...(gemsDir ? ["# Gems Collection's OBJ_* files; `# from` says which."] : []),
];
for (const g of out) {
    const objs = [...g.set].filter((m) => !seen.has(m)).sort((a, b) => a - b);
    if (!objs.length) continue;
    objs.forEach((m) => seen.add(m));
    lines.push('', `group ${g.name}`, `# from ${g.from.join(', ')}`);
    for (let i = 0; i < objs.length; i += 16) lines.push(objs.slice(i, i + 16).map((m) => m.toString(16).padStart(3, '0')).join(' '));
}
process.stdout.write(lines.join('\n') + '\n');
console.error(`dc_mdlgroups: ${out.length} groups, ${seen.size} objects`);
