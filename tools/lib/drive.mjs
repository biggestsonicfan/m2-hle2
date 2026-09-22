/*
 * drive.mjs — get the board from a cold boot into a game state.
 *
 * The scripted coin and buttons every fight-driving tool here plays, in one
 * place, plus the run-to-an-address loop around them.
 *
 * A warning worth keeping, because it looks like the obvious improvement and
 * is not: do NOT drive a fight frame by frame off a breakpoint on the frame
 * hook, the way ab-builds counts frames. The hook fires BEFORE the instruction
 * that ends the frame, so each game frame then takes two slices — and a slice
 * charges the board timers a whole frame each (emu_timers_slice_begin), so the
 * board's timers run at twice the video rate. (The sound board is charged at
 * the frame edge now, emu_sound_slice_end, and no longer skews this way; the
 * frozen timers still do.) Attract and character select are timed off those clocks, and
 * a coin script written for a free-running board never gets through: measured
 * here, 12,000 frames of coins and Start with the game still in attract.
 * ab-builds gets away with it because it only hashes, and both of its builds
 * are skewed alike.
 *
 * So the board free-runs, and the entry frame wanders by a few frames between
 * runs. A benchmark takes that into account by dividing by the instructions
 * executed rather than trusting the window to be the same work twice
 * (bench-state.mjs).
 */
import { COIN1, IN } from './board.mjs';

/* Attract to fight: a coin every ten seconds, then Start and the two buttons
 * character select wants, keyed to the frame number. */
export function fightInputAt(frame) {
    if (frame <= 300) return 0;
    let want = 0;
    if (frame % 600 < 8) want |= COIN1;
    const m = frame % 60;
    if (m < 6) want |= IN.START1;
    if (m >= 20 && m < 26) want |= IN.P1_B1;
    if (m >= 40 && m < 46) want |= IN.P1_B2;
    return want;
}

export const attractInputAt = () => 0;

/**
 * Free-run the board, playing `inputAt`, until `addr` is executed.
 * The emulator is left stopped on it.
 * @returns {Promise<number>} roughly the frame the state was entered on
 */
export async function driveToAddr(emu, addr, {
    inputAt = fightInputAt, maxFrames = 12000, timeoutMs = 20000,
} = {}) {
    await emu.clearAllBreakpoints();
    await emu.setBreakpoint(addr, 'state');
    let held = -1;
    for (let fc = 0; fc < maxFrames; fc += 2) {
        const want = inputAt(fc);
        if (want !== held) { await emu.setInput(want); held = want; }
        const w = await emu.waitFrames(2, timeoutMs);
        if (!w.reached) {
            const st = await emu.status();
            if (!st.running && Number(st.ip) === addr) {
                if (held !== 0) await emu.setInput(0);
                return fc;
            }
            if (!st.running) throw new Error(`the board stopped at ${st.ip}, not at 0x${addr.toString(16)}`);
        }
    }
    throw new Error(`0x${addr.toString(16)} was never reached in ${maxFrames} frames`);
}
