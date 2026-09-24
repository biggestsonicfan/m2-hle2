/*
 * ab-builds.mjs — do two builds emulate the same board?
 *
 * grade-reset.mjs's pattern, pointed at two executables instead of two boots:
 * count frames with a breakpoint on the frame hook so both stop on the SAME
 * instruction, then hash the registers and the nine regions the i960 writes.
 *
 *   node tools/ab-builds.mjs <exeA> <exeB> [--marks 600,1800] [--rom <zip>] [--sound]
 *
 * --sound adds the sound board at every mark: all of sound RAM, the SCSP's
 * register file and sound_status (the 68000's PC, SR and clock, the sample
 * count, interrupts taken, voices keyed and playing). The i960 regions alone
 * cannot see it, and a sound-board change has to hold both.
 */
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { M2Hle } from './lib/m2hle.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['marks', 'rom', 'port']);
const [exeA, exeB] = process.argv.slice(2).filter((a) => !a.startsWith('--') && /\.exe$|m2hle$/i.test(a));
if (!exeA || !exeB) { console.error('usage: node tools/ab-builds.mjs <exeA> <exeB> [--marks 600,1800]'); process.exit(2); }
const MARKS = args.str('marks', '600,1800').split(',').map(Number);
const ROM = path.resolve(args.str('rom', 'C:/Users/bigge/source/repos/ai/m2-hle2/test_m2snake/sfight.zip'));
const PORT = args.num('port', 7311);
const FRAME_HOOK = 0x11a04;

const REGIONS = [
    ['RAM2', 0x00200000, 0x000cf218], ['RAM', 0x00500000, 0x00100000],
    ['BUFF_RAM', 0x00900000, 0x00020000], ['TILE', 0x01000000, 0x00080000],
    ['TMAPGFX', 0x01080000, 0x00080000], ['PALETTE', 0x01800000, 0x00004000],
    ['COLORXLAT', 0x01810000, 0x0000c000], ['TEXRAM0', 0x11000000, 0x00100000],
    ['TEXRAM1', 0x11200000, 0x00100000],
];
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex').slice(0, 16);
process.env.M2HLE_UNTHROTTLE = '1';
const SOUND = args.bool('sound');

async function soundBoard(emu) {
    const ram = [], regs = [];
    for (let a = 0; a < 0x80000; a += 256) ram.push(...(await emu.rpc('read_wave', { addr: a, len: 256 })).b);
    for (let a = 0; a < 0xF00; a += 256) regs.push(...(await emu.rpc('read_comm', { addr: a, len: 256 })).b);
    const st = await emu.rpc('sound_status');
    delete st.out_fill;   /* how far the host has drained its ring, not board state */
    return { SND_RAM: sha(Buffer.from(ram)), SCSP_REGS: sha(Buffer.from(regs)), SND_STATUS: sha(JSON.stringify(st)) };
}

/* Its own directory per build, so neither shares m2hle.log with the other or
 * with the user's running instances. */
async function boards(exe, tag, port) {
    const dir = path.join(os.tmpdir(), 'm2hle-ab-' + tag);
    fs.mkdirSync(dir, { recursive: true });
    for (const z of ['sfight.zip', 'schamp.zip']) {
        const src = path.join(path.dirname(ROM), z);
        if (fs.existsSync(src)) fs.copyFileSync(src, path.join(dir, z));
    }
    const emu = await M2Hle.launch({ exe, rom: path.join(dir, 'sfight.zip'), port, run: false });
    const out = {};
    try {
        await emu.waitForRom();
        await emu.setBreakpoint(FRAME_HOOK, 'frame');
        let seen = 0;
        for (const mark of MARKS) {
            for (; seen < mark; seen++) {
                await emu.run();
                const r = await emu.waitForStop(120000);
                if (!r.stopped || r.reason !== 'breakpoint')
                    throw new Error(`${tag}: never reached frame ${seen + 1} (${JSON.stringify(r)})`);
            }
            const b = { registers: sha(JSON.stringify(await emu.registers())) };
            for (const [name, addr, size] of REGIONS)
                b[name] = sha((await emu.dumpRegion(addr, size, path.join(dir, `${name}.bin`))).bytes);
            if (SOUND) Object.assign(b, await soundBoard(emu));
            out[mark] = b;
            console.log(`${tag} @${mark}  ` + Object.entries(b).map(([k, v]) => `${k}=${v}`).join(' '));
        }
    } finally { await emu.close(); }
    return out;
}

const A = await boards(exeA, 'a', PORT);
const B = await boards(exeB, 'b', PORT + 1);
let bad = 0;
for (const mark of MARKS)
    for (const k of Object.keys(A[mark]))
        if (A[mark][k] !== B[mark][k]) { console.log(`DIFF @${mark} ${k}: ${A[mark][k]} vs ${B[mark][k]}`); bad++; }
console.log(bad === 0 ? `IDENTICAL at frames ${MARKS.join(', ')}` : `${bad} differences`);
process.exit(bad === 0 ? 0 : 1);
