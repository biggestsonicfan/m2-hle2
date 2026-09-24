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
 * clock on that frame_counter and the carpet's flight on the stage object's
 * age (the explorer has one clock; the board runs the ripple, flames and rings
 * off frame_counter and the flight off the age, 1160 frames apart on this
 * replay), rides the carpet so the scene is in the board's
 * frame, stands the camera where the board's stood, and renders at 496x384
 * three times: as drawn, with the plate hidden, and the rug alone. The board's
 * answer is that the plate covers none of the rug, so a pixel of the rug it
 * changes is a failure, and MAME's snapshot says which picture is the board's
 * there — how much of the rug's pattern shows in those pixels, against each
 * render. The plate showing past the rug's edge, where the ripple lifts it off
 * the plane, is in the board's picture too, and is only counted.
 *
 * The projection is the board's. camera_init sends the GEO focal lengths of
 * zoom (camera +0x13C) times focus_dist (0x501084/8, 280 by its own default),
 * and the full-screen window is centred on the 496x384 picture, so the
 * vertical field of view is 2 atan(192 / 280), 68.9 degrees. Zoom stays 1 and
 * roll 0 through the replay fight, which is why the camera record can leave
 * both out; --focal takes another focal length. The rug then registers
 * with MAME's, and so does the desert: the pattern IoU on the rug's pixels and
 * the sky's off it measure how well. They stop short of pixel for pixel: the
 * fighters and the HUD are not drawn here and cover part of both in MAME's
 * snapshot, so a perfect IoU is not 1. The rug's ground and the
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
 *   node tools/grade-carpet.mjs [--frames 400,490] [--focal 280] [--flight-lag 323] [--out DIR]
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

const args = parseArgs(['ref', 'frames', 'focal', 'flight-lag', 'out']);
const REF = path.resolve(args.str('ref', path.join(os.tmpdir(), 'm2hle-zsort', 'stage1')));
const OUT = args.str('out') ? path.resolve(args.str('out')) : null;
const W = 496, H = 384;
/* The board's focal length in pixels, and the vertical field of view it gives the picture. */
const FOCAL = args.num('focal', 280);
const FOV = 2 * Math.atan(H / 2 / FOCAL) * 180 / Math.PI;
/* The carpet's flight clock at MAME's record n is n - FLIGHT_LAG. Measured on this emulator's
 * replay: fa_object0_ram's age (+6) runs (board frame - jump) - 323, and the frame a mark
 * reads was flown at the age before (object_cont steps it after), so record n = jump + 1 + n
 * flies at n - 323. carpetAt at that clock is stage_xpos/ypos/zpos to the last digit. */
const FLIGHT_LAG = args.num('flight-lag', 323);
const PLATE = 3332;
/* A pixel of the rug's pattern rather than its ground. */
const PATTERN_RED = 80;
/* The pattern IoU that says the rug is where MAME's is. */
const REGISTERED = 0.45;
/* A pixel of sky rather than sand, pyramid or post; and the HUD's rows, which MAME draws over it. */
const isSky = (P, i) => P[i + 2] >= P[i] + 60;
const HUD_ROWS = 90;
/* The sky IoU that says the desert is where MAME's is. */
const DESERT = 0.6;
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
        flight: n - FLIGHT_LAG,
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
 * flight on the carpet object's age, the camera where the board's stood, and three renders at
 * the board's size. */
async function renderFrame({ f, W, H, FOV, PLATE }) {
    const s = window.stf, v = s.viewer;
    const T = await import('three');
    const D = await import('/js/display.js'), S = await import('/js/stages.js');
    const raf = () => new Promise((r) => requestAnimationFrame(() => r()));
    /* The explorer runs every draw off one frame; the board runs the flight off the object's
     * age. A draw whose ops open with the world prologue is flown, so it reads its frame that
     * much earlier; the ripple, the flames and the rings keep frame_counter. */
    const stage = s.stages[s.stageIndex];
    const prologue = JSON.stringify(D.stageWorldFrame(stage, 100, s.frames));
    const n = D.stageWorldFrame(stage, 100, s.frames).length;
    const lead = f.fc - f.flight;
    for (const { entry } of s.anim.entries) {
        const ops = entry.flownOps ?? entry.ops;
        if (typeof ops !== 'function' || JSON.stringify(ops(100).slice(0, n)) !== prologue) continue;
        entry.flownOps = ops;
        entry.ops = (g) => ops(g - lead);
    }
    /* Half a frame into fc, and held there; -1 makes the explorer step even on a frame it has. */
    window.__clock = 1e7;
    s.anim.start = 1e7 - ((f.fc + 0.5) / 60) * 1000;
    s.anim.frame = -1;
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
    /* The sun is built against the heading, so it is flown too. Nothing steps it again while
     * the frame is held. */
    v.material.uniforms.uLight.value.set(
        ...S.stageLight(stage.bright, stage.vecter[0], D.stageLightYaw(stage, f.flight, s.frames)));
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
    rep.note(`${frames.length} of MAME's frames, replay ${frames[0].n}..${frames.at(-1).n}, at focal ${FOCAL} (fov ${FOV.toFixed(2)})`);

    const totals = { changed: 0, over: 0, drawn: 0, hidden: 0, mame: 0, both: 0, either: 0, skyBoth: 0, skyEither: 0 };
    let held = 0, worst = null;
    for (const f of frames) {
        const shot = await page.evaluate(renderFrame, { f, W, H, FOV, PLATE });
        if (shot.frame === f.fc) held++;
        const [A, B, RA, RB] = [shot.drawn, shot.hidden, shot.rugA, shot.rugB].map((b) => Buffer.from(b, 'base64'));
        const M = readPng(path.join(REF, 'mame', `r${String(f.n).padStart(5, '0')}.png`));
        if (M.width !== W || M.height !== H) throw new Error(`MAME's snapshot is ${M.width}x${M.height}, not ${W}x${H}`);
        const same = (P, Q, i) => P[i] === Q[i] && P[i + 1] === Q[i + 1] && P[i + 2] === Q[i + 2];
        const pat = (P, i) => P[i] >= PATTERN_RED;
        const r = { changed: 0, over: 0, drawn: 0, hidden: 0, mame: 0, both: 0, either: 0, skyBoth: 0, skyEither: 0 };
        const mask = Buffer.alloc(W * H * 3);
        for (let i = 0; i < W * H * 3; i += 3) {
            const rug = same(RA, RB, i);
            /* Registration: on the rug's pixels, the pattern here against MAME's. */
            if (rug && (pat(A, i) || pat(M.rgb, i))) {
                r.either++;
                if (pat(A, i) && pat(M.rgb, i)) r.both++;
            }
            /* The desert: off the rug and under the HUD, the sky here against MAME's. */
            if (!rug && i >= HUD_ROWS * W * 3 && (isSky(A, i) || isSky(M.rgb, i))) {
                r.skyEither++;
                if (isSky(A, i) && isSky(M.rgb, i)) r.skyBoth++;
            }
            const hit = !same(A, B, i), over = hit && rug;
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
            ` (${r.changed - r.over} past its edge), pattern IoU ${(r.both / r.either).toFixed(3)}, sky IoU ${(r.skyBoth / r.skyEither).toFixed(3)}` +
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
    /* 0.515 at the board's focal length, 0.30-0.32 a 15-pixel step either side, 0.23 at the
     * 58 degrees once fitted by eye: a camera or a projection that has slipped falls under this. */
    const iou = totals.both / totals.either;
    rep.check('the rug registers with MAME\'s', iou >= REGISTERED,
        `pattern IoU ${iou.toFixed(3)} on the rug's pixels, at least ${REGISTERED} wanted (the fighters and the HUD stand over it in MAME's)`);
    /* 0.707 with the flight on the carpet's age, 0.697 four frames either side, 0.254 flown on
     * frame_counter as the explorer's one clock would have it. */
    const sky = totals.skyBoth / totals.skyEither;
    rep.check('the desert registers with MAME\'s', sky >= DESERT,
        `sky IoU ${sky.toFixed(3)} off the rug and under the HUD, at least ${DESERT} wanted`);
    if (pageErrors.length) rep.note(`page errors: ${pageErrors.slice(0, 3).join('; ')}`);
    if (OUT) rep.note(`pictures (MAME | explorer | magenta: plate over rug, cyan: past it) in ${OUT}`);
} finally {
    await browser.close();
    server.close();
}
rep.finish();
