/*
 * grade-carpet.mjs — the Flying Carpet's rug in the explorer, against MAME's
 * pictures, from the board's own camera.
 *
 * draw_sphynx_head lays one flat plate (model 3332) at y = 0 under the carpet,
 * as wide as the rug, and the rug's floor (flying_carpet_floor_anim, 594..657)
 * ripples a tenth of a unit either side of that plane. The board never compares
 * the two a pixel at a time: each polygon takes one z, the plate and every strip
 * of the rug ask for their farthest corner, and the plate's is the rug's far
 * edge, so it sorts behind every strip and MAME never shows it. A depth buffer
 * with a bounded recede does not get there on its own. Where the plate is
 * deeper along the view than the bound it keeps its own depth, the strips
 * step back to their far corners, and the troughs of the ripple fall behind the
 * plate: the rug goes flat, its ground colour with the pattern gone (noclip
 * issue 23).
 *
 * MAME is the oracle. grade-zsort's `--mame --stage 1` run leaves snapshots of
 * attract's replay fight on the Flying Carpet, and beside each the board
 * camera (0x519E98: eye, then pitch and yaw at 65536 a turn) and the
 * frame_counter. For each, this loads the explorer headless, holds its stage
 * clock on that frame_counter, rides the carpet so the scene is in the board's
 * frame, stands the camera where the board's stood, and renders at 496x384
 * three times: as drawn, with the plate hidden, and the rug alone. The board's
 * answer is that the plate covers none of the rug, so a pixel of the rug it
 * changes is a failure, and MAME's snapshot says which picture is the board's
 * there — how much of the rug's pattern shows in those pixels, against each
 * render. The plate showing past the rug's edge, where the ripple lifts it off
 * the plane, is in the board's picture too, and is only counted.
 *
 * The comparison is not pixel for pixel. The field of view is fitted by eye
 * (--fov, 58 degrees on the corner posts and the rug's far edge), the desert
 * stands differently because the explorer's flight is not on the board's
 * phase, and the fighters are not drawn here at all. The rug's ground and the
 * plate are one colour — (38, 0, 0) in MAME, (39, 0, 0) here — so what tells
 * them apart is the pattern: its red outline and gold, anything with red at
 * 80 or more.
 *
 * The explorer runs in a headless browser: puppeteer-core as node resolves it
 * from $M2_PUPPETEER, the explorer or ../noclip, driving Edge with SwiftShader.
 * $M2_BROWSER names another Chromium. A checkout without the package, or a
 * machine without the browser, skips.
 *
 *   node tools/grade-zsort.mjs --mame --stage 1     # MAME's snapshots, once (~12 min)
 *   node tools/grade-carpet.mjs [--frames 400,490] [--fov 58] [--out DIR]
 *
 * $M2_NOCLIP picks the explorer checkout, as every grader here.
 */
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';
import { NOCLIP, REPO } from './lib/noclip.mjs';
import { findRom } from './lib/rom.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';
import { readPng, writePng } from './lib/png.mjs';

const args = parseArgs(['ref', 'frames', 'fov', 'out']);
const REF = path.resolve(args.str('ref', path.join(os.tmpdir(), 'm2hle-zsort', 'stage1')));
const OUT = args.str('out') ? path.resolve(args.str('out')) : null;
const FOV = args.num('fov', 58);
const W = 496, H = 384;
const PLATE = 3332;
/* A pixel of the rug's pattern rather than its ground. */
const PATTERN_RED = 80;
const rep = new Report(`grade-carpet — the Flying Carpet's rug against MAME (explorer ${NOCLIP})`);

/* ---- MAME's side ------------------------------------------------------------ */

/* grade-zsort's record: frame_counter, stage, step, 16 bytes of each fighter, the camera. */
const MR_ROB = 0x10, CAMERA_LEN = 0x10, REC = 8 + 2 * MR_ROB + CAMERA_LEN;
const refBin = path.join(REF, 'mame.bin');
if (!fs.existsSync(refBin) || !fs.existsSync(path.join(REF, 'mame'))) {
    rep.skip('the MAME reference is present', `no ${refBin} — take one with node tools/grade-zsort.mjs --mame --stage 1`);
    rep.finish();
    process.exit();
}
const bin = fs.readFileSync(refBin);
const shots = fs.readdirSync(path.join(REF, 'mame')).map((f) => /^r(\d+)\.png$/.exec(f)).filter(Boolean)
    .map((m) => +m[1]).sort((a, b) => a - b);
const want = args.str('frames')?.split(',').map(Number) ?? shots;
const frames = want.filter((n) => shots.includes(n) && (n + 1) * REC <= bin.length).map((n) => {
    const o = n * REC, c = o + 8 + 2 * MR_ROB;
    return {
        n, fc: bin.readUInt32LE(o), stage: bin[o + 4],
        eye: [bin.readFloatLE(c), bin.readFloatLE(c + 4), bin.readFloatLE(c + 8)],
        pitch: bin.readInt16LE(c + 12), yaw: bin.readInt16LE(c + 14),
    };
});
const offStage = frames.filter((f) => f.stage !== 1);
if (!rep.check('MAME\'s snapshots are of the Flying Carpet', frames.length > 0 && !offStage.length,
    `${frames.length} frames${offStage.length ? `, ${offStage.length} on another stage` : ''}`)) {
    rep.finish();
    process.exit();
}

/* ---- The explorer's side ---------------------------------------------------- */

/* Resolved the way node would from each of these, so a worktree without node_modules of its own
 * finds its parent's. */
function loadPuppeteer() {
    for (const base of [process.env.M2_PUPPETEER, NOCLIP, path.resolve(REPO, '..', 'noclip'), path.join(REPO, 'tools')]) {
        if (!base) continue;
        try {
            return createRequire(path.join(path.resolve(base), 'package.json'))('puppeteer-core');
        } catch { /* not from here */ }
    }
    return null;
}
const puppeteer = loadPuppeteer();
const browserExe = process.env.M2_BROWSER ?? 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
if (!puppeteer || !fs.existsSync(browserExe)) {
    rep.skip('a headless browser to run the explorer in',
        !puppeteer ? `no puppeteer-core from $M2_PUPPETEER, ${NOCLIP} or ../noclip` : `no browser at ${browserExe} — set $M2_BROWSER`);
    rep.finish();
    process.exit();
}

const ROOT = path.normalize(NOCLIP);
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png' };
const server = http.createServer((req, res) => {
    const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    const file = path.normalize(path.join(ROOT, p === '/' ? 'index.html' : p));
    if (file.endsWith('.zip') || !file.startsWith(ROOT) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
        res.writeHead(404); res.end(); return;
    }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] ?? 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
}).listen(0);

/* One frame, in the page: the explorer's stage clock held on the board's frame_counter, the
 * camera where the board's stood, and three renders at the board's size. */
async function renderFrame({ f, W, H, FOV, PLATE }) {
    const s = window.stf, v = s.viewer;
    const T = await import('three');
    const raf = () => new Promise((r) => requestAnimationFrame(() => r()));
    /* Half a frame into fc, and held there. */
    window.__clock = 1e7;
    s.anim.start = 1e7 - ((f.fc + 0.5) / 60) * 1000;
    await raf(); await raf();
    /* The board's camera is in the carpet's frame, and so is the scene while the carpet is
     * ridden. The explorer negates z on read; yaw = -atan2(dx, dz), pitch = atan2(dy, across). */
    const yaw = (f.yaw / 65536) * 2 * Math.PI, pitch = (f.pitch / 65536) * 2 * Math.PI;
    const d = [-Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch)];
    const cam = v.camera;
    const place = () => {
        cam.position.set(f.eye[0], f.eye[1], -f.eye[2]);
        cam.up.set(0, 1, 0);
        cam.lookAt(f.eye[0] + d[0], f.eye[1] + d[1], -(f.eye[2] + d[2]));
    };
    place();
    v.fly?.syncFromCamera?.();
    /* Let the billboards and the sky turn to it, then put it back exactly. */
    await raf(); await raf();
    place();
    const keep = { fov: cam.fov, aspect: cam.aspect };
    cam.fov = FOV; cam.aspect = W / H; cam.updateProjectionMatrix();
    const rt = new T.WebGLRenderTarget(W, H);
    const grab = () => {
        v.renderer.setRenderTarget(rt);
        v.renderer.render(v.scene, cam);
        const px = new Uint8Array(W * H * 4);
        v.renderer.readRenderTargetPixels(rt, 0, 0, W, H, px);
        v.renderer.setRenderTarget(null);
        let out = '';
        const row = new Uint8Array(W * 3);
        for (let y = H - 1; y >= 0; y--) {
            for (let x = 0; x < W; x++) for (let k = 0; k < 3; k++) row[x * 3 + k] = px[(y * W + x) * 4 + k];
            for (let i = 0; i < row.length; i += 0x2000) out += String.fromCharCode(...row.subarray(i, i + 0x2000));
        }
        return btoa(out);
    };
    const plates = [];
    v.scene.traverse((o) => { if (o.isMesh && o.userData.modelIndex === PLATE) plates.push(o); });
    const drawn = grab();
    for (const p of plates) p.visible = false;
    const hidden = grab();
    for (const p of plates) p.visible = true;
    /* The rug alone, over two clear colours: a pixel that comes out the same over both is one
     * the rug covers. */
    const others = [];
    v.scene.traverse((o) => {
        if (o.visible && (o.isMesh || o.isLine || o.isPoints || o.isSprite) && o.userData.layer !== 'platform') {
            others.push(o); o.visible = false;
        }
    });
    const bg = v.scene.background, cc = v.renderer.getClearColor(new T.Color()), ca = v.renderer.getClearAlpha();
    v.scene.background = null;
    v.renderer.setClearColor(0x000000, 1);
    const rugA = grab();
    v.renderer.setClearColor(0xffffff, 1);
    const rugB = grab();
    v.scene.background = bg;
    v.renderer.setClearColor(cc, ca);
    for (const o of others) o.visible = true;
    rt.dispose();
    cam.fov = keep.fov; cam.aspect = keep.aspect; cam.updateProjectionMatrix();
    return { drawn, hidden, rugA, rugB, frame: s.anim.frame, plates: plates.length };
}

const browser = await puppeteer.launch({
    executablePath: browserExe,
    headless: true,
    args: ['--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--use-angle=swiftshader'],
});
try {
    const page = await browser.newPage();
    await page.setViewport({ width: 1120, height: 640 });
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await page.goto(`http://127.0.0.1:${server.address().port}/?desktop#game=sfight&tab=stage&stage=1&cam=fly`);
    await page.evaluate(() => {
        const a = document.querySelector('#loader-ack');
        a.checked = true;
        a.dispatchEvent(new Event('change'));
    });
    await (await page.$('#loader-file')).uploadFile(...findRom().zips);
    await page.waitForFunction(() => window.stf?.rom && window.stf.anim?.entries?.length, { timeout: 180000 });
    await page.evaluate(() => {
        /* The stage clock reads performance.now(); a held clock is a frame that stays put. */
        const real = performance.now.bind(performance);
        window.__clock = null;
        performance.now = () => window.__clock ?? real();
        const ride = document.querySelector('#opt-ride');
        if (!ride.checked) ride.click();
    });
    rep.note(`${frames.length} of MAME's frames, replay ${frames[0].n}..${frames.at(-1).n}, at fov ${FOV}`);

    const totals = { changed: 0, over: 0, drawn: 0, hidden: 0, mame: 0 };
    let held = 0, worst = null;
    for (const f of frames) {
        const shot = await page.evaluate(renderFrame, { f, W, H, FOV, PLATE });
        if (shot.frame === f.fc) held++;
        const [A, B, RA, RB] = [shot.drawn, shot.hidden, shot.rugA, shot.rugB].map((b) => Buffer.from(b, 'base64'));
        const M = readPng(path.join(REF, 'mame', `r${String(f.n).padStart(5, '0')}.png`));
        if (M.width !== W || M.height !== H) throw new Error(`MAME's snapshot is ${M.width}x${M.height}, not ${W}x${H}`);
        const same = (P, Q, i) => P[i] === Q[i] && P[i + 1] === Q[i + 1] && P[i + 2] === Q[i + 2];
        const pat = (P, i) => P[i] >= PATTERN_RED;
        const r = { changed: 0, over: 0, drawn: 0, hidden: 0, mame: 0 };
        const mask = Buffer.alloc(W * H * 3);
        for (let i = 0; i < W * H * 3; i += 3) {
            const hit = !same(A, B, i), over = hit && same(RA, RB, i);
            /* The drawn picture, dimmed: magenta where the plate covers the rug, cyan past it. */
            mask[i] = over ? 255 : hit ? 0 : A[i] >> 2;
            mask[i + 1] = over ? 0 : hit ? 255 : A[i + 1] >> 2;
            mask[i + 2] = hit ? 255 : A[i + 2] >> 2;
            if (!hit) continue;
            r.changed++;
            if (!over) continue;
            r.over++;
            if (pat(A, i)) r.drawn++;
            if (pat(B, i)) r.hidden++;
            if (pat(M.rgb, i)) r.mame++;
        }
        for (const k in r) totals[k] += r[k];
        if (!worst || r.over > worst.r.over) worst = { f, r };
        const pc = (k) => (100 * r[k] / r.over).toFixed(1) + '%';
        rep.note(`replay ${String(f.n).padStart(4)}  fc ${f.fc}  the plate covers ${String(r.over).padStart(6)} px of the rug` +
            ` (${r.changed - r.over} past its edge)` +
            (r.over ? `; pattern there: drawn ${pc('drawn')}, plate hidden ${pc('hidden')}, MAME ${pc('mame')}` : ''));
        if (OUT) {
            fs.mkdirSync(OUT, { recursive: true });
            const tag = String(f.n).padStart(5, '0');
            /* MAME | drawn | the plate's pixels. */
            const side = Buffer.alloc(W * 3 * H * 3);
            for (let y = 0; y < H; y++) {
                M.rgb.copy(side, y * W * 9, y * W * 3, (y + 1) * W * 3);
                A.copy(side, y * W * 9 + W * 3, y * W * 3, (y + 1) * W * 3);
                mask.copy(side, y * W * 9 + W * 6, y * W * 3, (y + 1) * W * 3);
            }
            writePng(path.join(OUT, `carpet-r${tag}.png`), { width: W * 3, height: H, rgb: side });
        }
    }

    rep.check('the explorer\'s stage clock held on MAME\'s frame_counter', held === frames.length,
        `${held} of ${frames.length} frames`);
    rep.check('the plate is drawn', worst !== null && totals.changed > 0,
        `it changes ${totals.changed} px in all, ${totals.changed - totals.over} of them past the rug's edge, as the board's plate shows`);
    const pc = (k) => (100 * totals[k] / totals.over).toFixed(1) + '%';
    rep.check('the plate covers none of the rug, as MAME\'s is behind every strip', totals.over === 0,
        totals.over === 0 ? `0 px over ${frames.length} frames`
            : `${totals.over} px over ${frames.length} frames, worst replay ${worst.f.n} (${worst.r.over} px); ` +
              `pattern in those pixels: drawn ${pc('drawn')}, plate hidden ${pc('hidden')}, MAME ${pc('mame')}`);
    if (pageErrors.length) rep.note(`page errors: ${pageErrors.slice(0, 3).join('; ')}`);
    if (OUT) rep.note(`pictures (MAME | explorer | magenta: plate over rug, cyan: past it) in ${OUT}`);
} finally {
    await browser.close();
    server.close();
}
rep.finish();
