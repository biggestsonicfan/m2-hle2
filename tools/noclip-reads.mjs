/*
 * noclip-reads.mjs — the explorer's reads of one set's model table, as the C
 * initializer tests/prepared_test.c holds a prepared profile to.
 *
 *   node tools/noclip-reads.mjs <set.zip> [parent.zip]
 *   node tools/noclip-reads.mjs dynmcopb dynamcop       # names, looked up in $ROMS_DIR
 *   node tools/noclip-reads.mjs --per-model hotd        # one line per mesh
 *
 * It copies the explorer's js/ (lib/noclip.mjs finds it) to a temp directory,
 * probes decodeModel there (what it derives from each texture header, what it
 * hands emitTri, each triangle's lighting normal and bit 128, and its totals),
 * wraps the textures ROM to log every index read, then walks every entry the
 * way the test does and prints the noclip_reads_t initializer: the entries'
 * FNV-1a, the first records', the decode, header, triangle and lighting
 * mixes, in the order the test's decode_mix* functions mix them. The four
 * counts at the end (cull_diff, light_dir, light_zero, light_len) are the
 * board's rule against noclip's, which the test computes itself: leave them
 * 0, run the test, and copy the counts it reports.
 *
 * The line before the initializer says which game the explorer took the set
 * for, where its table is, the longest mesh (GEO3D_IA_MAX_VPS must cover it)
 * and whether the game lights by the plane of A, B and C. When a total
 * disagrees, --per-model prints the records, faces, textured faces and
 * triangles of every mesh, as PREPARED_DUMP=1 makes the test print its own.
 *
 * The probes are text patches on model.js at known lines; when the explorer
 * moves, this script says which anchor it lost. NOCLIP-SYNC.md §8.2.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { NOCLIP, noclipVersion } from './lib/noclip.mjs';

/* --per-model: one line per mesh (index, offset, records, faces, textured,
 * triangles), the lines prepared_test prints with PREPARED_DUMP=1. */
const perModel = process.argv.includes('--per-model');
const args = process.argv.slice(2).filter((a) => a !== '--per-model');
if (args.length < 1 || args.length > 2) {
    console.error('usage: node tools/noclip-reads.mjs [--per-model] <set.zip|set> [parent.zip|parent]');
    process.exit(2);
}
const zipPath = (a) => {
    if (fs.existsSync(a)) return a;
    const dir = process.env.ROMS_DIR;
    const c = dir ? path.join(dir, a.endsWith('.zip') ? a : a + '.zip') : null;
    if (c && fs.existsSync(c)) return c;
    throw new Error(`${a}: not a file, and not in $ROMS_DIR`);
};
const zips = args.map(zipPath);

/* A probed copy of the explorer's js/, gone when this process ends. */
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'noclip-reads-'));
process.on('exit', () => fs.rmSync(tmp, { recursive: true, force: true }));
const src = path.join(NOCLIP, 'js'), dst = path.join(tmp, 'js');
fs.cpSync(src, dst, { recursive: true });
{
    const f = path.join(dst, 'model.js');
    let s = fs.readFileSync(f, 'utf8').replace(/\r\n/g, '\n');
    const rep = (a, b) => {
        const n = s.split(a).length - 1;
        if (n !== 1) throw new Error(`model.js: ${n} matches for anchor ${JSON.stringify(a)}; the probe needs a new one`);
        s = s.replace(a, b);
    };
    rep('    let material = 0;\n', '    let material = 0;\n    let pFi = 0;\n');
    rep('        const fi = i >> 2;\n', '        const fi = i >> 2;\n        pFi = fi;\n');
    rep('                if (palette[matidx] >= 0) bgr555(palette[matidx], rgb);\n',
        '                if (palette[matidx] >= 0) bgr555(palette[matidx], rgb);\n' +
        '                globalThis.__P?.hdr(fi, textured, faceFlags, texw, texh, lumaBase, tx, sheet, ty, matidx, tw, th);\n');
    rep('        triFaces.push(faceCount);\n',
        '        triFaces.push(faceCount);\n' +
        '        globalThis.__P?.tri({ sv, uvu, uvv, att, nrm, zMode, zSrc, facePt, material, fi: pFi, p0, p1, p2, s0, s1, s2 });\n');
    rep('        nx /= nl; ny /= nl; nz /= nl;\n',
        '        nx /= nl; ny /= nl; nz /= nl;\n        globalThis.__P?.lit(nx, ny, nz, (faceFlags & 128) ? 1 : 0);\n');
    rep('    if (positions.length === 0) return null;\n',
        '    globalThis.__P?.done(qt.length, faceCount, texturedFaces, paletteMax, triFaces, positions.length === 0);\n' +
        '    if (positions.length === 0) return null;\n');
    fs.writeFileSync(f, s);
}
const { loadRomSet, readModelEntry, meshOffsetOf, facePalette } = await import(pathToFileURL(path.join(dst, 'romset.js')).href);
const { decodeModel } = await import(pathToFileURL(path.join(dst, 'model.js')).href);

const readAB = (p) => { const b = fs.readFileSync(p); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); };
const rom = await loadRomSet(zips.map(readAB));
const t = rom.game.modelTable;
const hex = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');
/* FNV-1a as the test's fnv_word / fnv_float: little-endian bytes, NaN as 0x7fc00000. */
const fnvw = (h, v) => { v >>>= 0; for (let b = 0; b < 4; b++) { h ^= (v >>> (b * 8)) & 0xff; h = Math.imul(h, 0x01000193) >>> 0; } return h; };
const f32 = new Float32Array(1), u32 = new Uint32Array(f32.buffer);
const fnvf = (h, f) => { if (f !== f) return fnvw(h, 0x7fc00000); f32[0] = f; return fnvw(h, u32[0]); };
const fnv3 = (h, a, i) => fnvf(fnvf(fnvf(h, a[i]), a[i + 1]), a[i + 2]);
const INIT = 0x811c9dc5;

/* The entries: every one's three words; samples 0, 1 and the last ROM-backed one. */
let h = INIT, meshes = 0, ram = 0;
const count = t.count;
let lastRom = count - 1;
while (lastRom > 1 && !(readModelEntry(rom, lastRom).meshPtr & 0x800000)) lastRom--;
const samp = [0, 1, lastRom];
const sample = [];
for (let i = 0; i < count; i++) {
    const e = readModelEntry(rom, i);
    h = fnvw(fnvw(fnvw(h, e.uvPtr), e.matPtr), e.meshPtr);
    if (e.meshPtr) { meshes++; if (!(e.meshPtr & 0x800000)) ram++; }
}
for (const i of samp) {
    const e = readModelEntry(rom, i);
    sample.push(`{ 0x${e.uvPtr.toString(16)}, 0x${e.matPtr.toString(16)}, 0x${e.meshPtr.toString(16)} }`);
}
const entFnv = h;

/* The first record of each ROM-backed mesh (meshOffsetOf, decodeModel's bound). */
const pv = rom.polygonsView, plen = rom.polygons.length;
h = INIT;
let found = 0, past = 0;
const msamp = [];
for (let i = 0; i < count; i++) {
    const e = readModelEntry(rom, i);
    if (!e.meshPtr || !(e.meshPtr & 0x800000)) continue;
    const off = meshOffsetOf(rom, e);
    if (off + 40 > plen) { past++; continue; }
    found++;
    h = fnvw(fnvw(h, i), off);
    for (let w = 0; w < 10; w++) h = fnvw(h, pv.getUint32(off + w * 4, true));
    if (samp.includes(i)) msamp.push(`{ ${i}, 0x${off.toString(16)}, 0x${pv.getUint32(off + 24, true).toString(16)} }`);
}
const meshFnv = h;

/* decodeModel on each, probed. */
facePalette(rom);
let reads = [], hdrs = [], tris = [], lits = [], done = null;
const real = rom.textures;
rom.textures = new Proxy(real, {
    get(tg, p) {
        const n = Number(p);
        if (p !== '' && Number.isInteger(n) && n >= 0) reads.push(n);
        return Reflect.get(tg, p);
    },
});
globalThis.__P = {
    hdr(...a) { hdrs.push(a); },
    tri(q) {
        /* A snapshot: the arrays are rewritten by the next record. */
        const { sv, uvu, uvv, att, nrm, zMode, zSrc, facePt, material, fi, p0, p1, p2, s0, s1, s2 } = q;
        const v3 = (a, i) => [a[i], a[i + 1], a[i + 2]];
        tris.push({ p: [v3(sv, p0), v3(sv, p1), v3(sv, p2)], u: [uvu[s0], uvu[s1], uvu[s2]], v: [uvv[s0], uvv[s1], uvv[s2]],
                    material, two: (att[fi] >>> 17) & 1, n: v3(nrm, fi * 3), zMode,
                    zc: zSrc.map((z) => v3(sv, z)), fp: v3(sv, facePt) });
    },
    lit(nx, ny, nz, b) { lits.push([nx, ny, nz, b]); },
    done(records, faces, textured, pmax, triFaces, isNull) {
        done = { records, faces, textured, pmax, triFaces: triFaces.slice(), isNull };
    },
};
let dn = 0, dnull = 0, drec = 0, dfaces = 0, dtex = 0, dfnv = INIT;
let hn = 0, htex = 0, hfnv = INIT, tn = 0, tfnv = INIT, b128 = 0, lfnv = INIT;
let maxrec = 0, maxi = -1;
for (let i = 0; i < count; i++) {
    const e = readModelEntry(rom, i);
    if (!e.meshPtr || !(e.meshPtr & 0x800000)) continue;
    const off = meshOffsetOf(rom, e);
    if (off + 40 > plen) continue;
    reads = []; hdrs = []; tris = []; lits = []; done = null;
    const m = decodeModel(rom, i);
    if (!done) throw new Error(`model ${i}: the done probe did not fire`);
    if ((m === null) !== done.isNull) throw new Error(`model ${i}: null and the probe disagree`);
    if (done.triFaces.length !== tris.length) throw new Error(`model ${i}: triangle count and the probe disagree`);
    if (perModel) console.log(`dump: ${i} 0x${off.toString(16).toUpperCase()} ${done.records} ${done.faces} ${done.textured} ${tris.length}`);
    /* decode (decode_mix) */
    let x = fnvw(fnvw(fnvw(dfnv, i), off), done.records);
    x = fnvw(x, reads.length);
    for (const r of reads) x = fnvw(x, r);
    dn++; drec += done.records;
    if (done.isNull) { dnull++; dfnv = fnvw(x, 0xFFFFFFFF); }
    else {
        x = fnvw(fnvw(fnvw(x, done.faces), done.textured), done.pmax);
        x = fnvw(x, done.triFaces.length);
        for (const f of done.triFaces) x = fnvw(x, f);
        dfaces += done.faces; dtex += done.textured; dfnv = x;
    }
    /* headers (decode_mix_headers) */
    x = fnvw(fnvw(hfnv, i), hdrs.length);
    for (const [fi, textured, fflags, texw, texh, luma, tx, sheet, ty, matidx, tw, th] of hdrs) {
        for (const v of [fi, textured ? 1 : 0, fflags, texw, texh, luma, tx, sheet, ty, matidx, tw, th]) x = fnvw(x, v);
        htex += textured ? 1 : 0;
    }
    hn += hdrs.length; hfnv = x;
    /* triangles (decode_mix_tris) */
    x = fnvw(tfnv, i);
    for (const q of tris) {
        for (const p of q.p) x = fnv3(x, p, 0);
        for (let c = 0; c < 3; c++) x = fnvf(fnvf(x, q.u[c]), q.v[c]);
        x = fnv3(fnvw(fnvw(x, q.material), q.two), q.n, 0);
        x = fnvw(x, q.zMode);
        for (const z of q.zc) x = fnv3(x, z, 0);
        x = fnv3(x, q.fp, 0);
    }
    tn += tris.length; tfnv = fnvw(x, tris.length);
    /* lighting (decode_mix_lit) */
    x = fnvw(lfnv, i);
    for (const [nx, ny, nz, b] of lits) { x = fnvw(fnvf(fnvf(fnvf(x, nx), ny), nz), b); b128 += b; }
    lfnv = fnvw(x, lits.length);
    if (done.records > maxrec) { maxrec = done.records; maxi = i; }
}
rom.textures = real;
const plane = rom.game.lighting?.planeNormals === true ? 1 : 0;
console.log(`noclip ${noclipVersion()} game ${rom.game.id ?? rom.game.name} table ${hex(t.offset ?? 0)} count ${count} ` +
            `stride ${t.stride} meshPtr -${hex(rom.game.meshPtr.subtract)} +${hex(rom.game.meshPtr.add)} ` +
            `longest ${maxrec} records (model ${maxi}) plane ${plane}`);
console.log(`      { ${count}, ${hex(entFnv)}u, ${meshes}, ${ram}, { ${samp.join(', ')} },`);
console.log(`        { ${sample.join(', ')} },`);
console.log(`        ${found}, ${past}, ${hex(meshFnv)}u, { ${msamp.join(', ')} },`);
console.log(`        ${dn}, ${dnull}, ${drec}, ${dfaces}, ${dtex}, ${hex(dfnv)}u, ${hn}, ${htex}, ${hex(hfnv)}u,`);
console.log(`        ${tn}, ${hex(tfnv)}u, ${plane}, ${b128}, ${hex(lfnv)}u, 0, 0, 0, 0 },`);
