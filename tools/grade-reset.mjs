/*
 * grade-reset.mjs — is the board a netplay session starts from the board a
 * power-on starts from?
 *
 * A session is a cold boot on both machines, because with no savestates that is
 * the only state two emulators can be certain to share (net/netplay.h). So the
 * reset at the barrier has one job: leave nothing behind. Whatever survives it —
 * a latch in the run loop, a static in an HLE hook, a region the re-init forgets
 * — is state one player has and the other does not, since no two players did
 * the same thing before they pressed Start. The frame check would not see it
 * either: it hashes the i960's registers and the step count, not RAM, so a
 * leftover that does not change control flow at once rides along until it does.
 *
 * This one needs no oracle. The emulator is its own: boot, run N frames, hash
 * the board; then play on into some other state, do the barrier's reset with no
 * session (`board_reset` over the bridge — the same code, netplay_reset_board_cb),
 * run N frames, hash again. The two must be equal byte for byte, from every
 * state tried, and the second reset must be as clean as the first:
 *
 *   boot   -> N frames -> A
 *   ... --pre frames of attract (the intro movie) ...
 *   reset  -> N frames -> must equal A
 *   ... further into attract (the replay fight) ...
 *   reset  -> N frames -> must equal A
 *
 * Frames are counted with a breakpoint on the frame hook, because a run stopped
 * "after N frames" by polling stops a few frames late and nothing would match.
 * Measured 2026-09-19 on master (dc9a646): exact, from both states.
 *
 * What it cannot see: anything outside the regions below (the GEO's private
 * RAM, the 68000's — the sound board is graded separately), and anything that
 * differs between two MACHINES rather than two boots on one.
 *
 *   node tools/grade-reset.mjs
 *   node tools/grade-reset.mjs --pre 1500,5300,9000 --frames 300 --port 7555
 */
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { Report } from './lib/report.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['pre', 'frames', 'port', 'out']);
const rep = new Report('grade-reset — the boot after a netplay reset, against a first boot');

const FRAME_HOOK = 0x11a04;   /* STF variable_diff_calc: the emulator's frame boundary */
const FRAMES = args.num('frames', 120);
const PRE = args.str('pre', '1500,5300').split(',').map(Number).filter((n) => n > 0);
const OUT = args.str('out', path.join(os.tmpdir(), 'm2hle-reset'));

/* Everything the i960 can write that the bridge can read back. */
const REGIONS = [
    ['RAM2',      0x00200000, 0x000cf218],
    ['RAM',       0x00500000, 0x00100000],
    ['BUFF_RAM',  0x00900000, 0x00020000],
    ['TILE',      0x01000000, 0x00080000],
    ['TMAPGFX',   0x01080000, 0x00080000],
    ['PALETTE',   0x01800000, 0x00004000],
    ['COLORXLAT', 0x01810000, 0x0000c000],
    ['TEXRAM0',   0x11000000, 0x00100000],
    ['TEXRAM1',   0x11200000, 0x00100000],
];

const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

/* Attract runs a lot faster than 60 Hz when nobody is watching it. */
process.env.M2HLE_UNTHROTTLE = '1';

const emu = await M2Hle.launch({ rom: findRom().primary, port: args.num('port', 7172), run: false });
try {
    const st0 = await emu.waitForRom();
    if (st0.profile !== 'sfight')
        throw new Error(`profile is '${st0.profile}': the frame hook address here is STF's`);

    /* Run to the FRAMES-th frame boundary after a boot or a reset, and take the board. */
    async function take(tag) {
        await emu.setBreakpoint(FRAME_HOOK, 'frame');
        for (let i = 0; i < FRAMES; i++) {
            await emu.run();
            const r = await emu.waitForStop(60000);
            if (!r.stopped || r.reason !== 'breakpoint')
                throw new Error(`${tag}: the board never reached frame ${i + 1} (${JSON.stringify(r)})`);
        }
        const board = { registers: sha(JSON.stringify(await emu.registers())) };
        const bytes = {};
        for (const [name, addr, size] of REGIONS) {
            const d = await emu.dumpRegion(addr, size, path.join(OUT, `${tag}-${name}.bin`));
            board[name] = sha(d.bytes);
            bytes[name] = d.bytes;
        }
        await emu.clearBreakpoint(FRAME_HOOK);
        return { board, bytes };
    }

    const first = await take('boot');
    rep.note(`first boot: ${FRAMES} frames, ${REGIONS.length} regions hashed`);

    for (const target of PRE) {
        /* On from wherever the last comparison stopped, to a state worth resetting from. */
        await emu.run();
        const w = await emu.waitFrames(target, 300000);
        await emu.stop();
        if (!w.reached) { rep.skip(`reset after ${target} frames`, 'the game did not get there'); continue; }

        const r = await emu.rpc('board_reset', {}, { allowFail: true });
        if (!r.ok) {
            rep.skip(`reset after ${target} frames`,
                     r.error?.includes('unknown') ? 'this build has no board_reset — rebuild it' : String(r.error));
            continue;
        }
        const st = await emu.status();
        rep.check(`reset ${r.resets}: the frame clock is back at zero`, st.frames === 0, `frames=${st.frames}`);

        const again = await take(`reset${r.resets}`);
        for (const key of Object.keys(first.board)) {
            const same = again.board[key] === first.board[key];
            let where = '';
            if (!same && first.bytes[key]) {
                const a = first.bytes[key], b = again.bytes[key];
                let at = 0, n = 0;
                for (let i = a.length - 1; i >= 0; i--) if (a[i] !== b[i]) { at = i; n++; }
                const base = REGIONS.find((x) => x[0] === key)[1];
                where = `${n} bytes differ, first at 0x${(base + at).toString(16).toUpperCase().padStart(8, '0')}`;
            }
            rep.check(`reset ${r.resets}, ${target} frames further on: ${key} as a first boot leaves it`, same, where);
        }
    }
    rep.note(`dumps in ${OUT}`);
} finally {
    await emu.close();
}
rep.finish();
