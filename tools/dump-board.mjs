/*
 * dump-board.mjs — take a scene's board state out of a running emulator.
 *
 * The capture itself is lib/capture.mjs; this is the command line over it.
 *
 *   node tools/dump-board.mjs out/board
 *   node tools/dump-board.mjs out/board --stage 0     # pin South Island
 *   node tools/dump-board.mjs out/board --attach      # emulator already up
 *
 * Writes texram0.bin, texram1.bin, lumaram.bin, colorxlat.bin and a
 * capture.json naming the scene they were taken from. Those are the game's own
 * data: the default output is a temp directory outside the checkout, and
 * .gitignore refuses them in it either way.
 */
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { captureBoard, DEFAULT_OUT } from './lib/capture.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['stage', 'port', 'polls', 'every']);
const out = args.positional[0] ?? DEFAULT_OUT;
const stage = args.num('stage', null);
const attach = args.bool('attach');
const port = args.num('port', 7172);

const emu = attach
    ? await M2Hle.attach({ port })
    : await M2Hle.launch({ rom: findRom().primary, port, run: true });

try {
    /* The bridge answers before the ROM has finished loading, so a status read
     * at connect time says ip=0 and frames=0 whatever the machine is doing. */
    const st = await emu.waitUntilRunning();
    console.log(`emulator up: profile=${st.profile} ip=${st.ip} frames=${st.frames}`);
    if (st.profile !== 'sfight') {
        console.error(`this tool reads Sonic The Fighters' scene state; profile is ${st.profile}`);
        process.exit(2);
    }

    const manifest = await captureBoard(emu, {
        out, stage,
        polls: args.num('polls', 40),
        every: args.num('every', 30),
        log: (s) => console.log('  ' + s),
    });

    if (!manifest.settled) {
        console.warn('warning: the upload was still moving when the dump was taken');
    }
    console.log(`\nstage ${manifest.stageNum} -> ${out}`);
} catch (e) {
    console.error('\n' + e.message);
    process.exitCode = 3;
} finally {
    await emu.close({ kill: !attach });
}
