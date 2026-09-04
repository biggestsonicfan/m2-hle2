/*
 * report.mjs — one shape for what a grader says.
 *
 * A grader is not a test: it does not assert that the emulator is right, it
 * measures how far it is from a second implementation and says so. So every
 * line carries a verdict *and* a measurement, and a run ends with an exit code
 * a script can branch on.
 *
 * Three verdicts, and the difference between the last two is the whole point:
 *
 *   PASS   the two agree to the tolerance stated on the line
 *   FAIL   they disagree, and the emulator is the side under test
 *   SKIP   the measurement could not be taken — the game was not in the right
 *          scene, a capture is missing, a region was never filled. A skip is
 *          not a pass. A grader that skips everything exits 0 and says so
 *          loudly, because a silent no-op that looks green is the one failure
 *          mode a grading harness must not have.
 */

const GREEN = '\x1b[32m', RED = '\x1b[31m', YELLOW = '\x1b[33m', DIM = '\x1b[2m', OFF = '\x1b[0m';
const colour = process.stdout.isTTY && !process.env.NO_COLOR;
const c = (code, s) => (colour ? code + s + OFF : s);

export class Report {
    constructor(title) {
        this.title = title;
        this.rows = [];
        this.t0 = Date.now();
        console.log('\n' + c(DIM, '─'.repeat(72)));
        console.log(title);
        console.log(c(DIM, '─'.repeat(72)));
    }

    note(msg) { console.log(c(DIM, '  ' + msg)); }

    /** A measurement with a verdict. `detail` is the number behind it. */
    check(name, ok, detail = '') {
        this.rows.push({ name, ok, detail });
        const tag = ok ? c(GREEN, 'PASS') : c(RED, 'FAIL');
        console.log(`  ${tag}  ${name}${detail ? '  ' + c(DIM, detail) : ''}`);
        return ok;
    }

    /** Could not be measured. Counted separately and never as a pass. */
    skip(name, why) {
        this.rows.push({ name, skipped: true, detail: why });
        console.log(`  ${c(YELLOW, 'SKIP')}  ${name}  ${c(DIM, why)}`);
    }

    get passed()  { return this.rows.filter((r) => !r.skipped && r.ok).length; }
    get failed()  { return this.rows.filter((r) => !r.skipped && !r.ok).length; }
    get skipped() { return this.rows.filter((r) => r.skipped).length; }

    /** Print the summary and exit with 0 (all measured checks passed) or 1. */
    finish() {
        const ms = Date.now() - this.t0;
        console.log(c(DIM, '─'.repeat(72)));
        const bits = [`${this.passed} passed`];
        if (this.failed)  bits.push(c(RED, `${this.failed} failed`));
        if (this.skipped) bits.push(c(YELLOW, `${this.skipped} skipped`));
        console.log(`  ${bits.join(', ')}  ${c(DIM, `(${(ms / 1000).toFixed(1)}s)`)}`);
        /* Only when nothing at all was measured. A run with failures measured
         * plenty; saying otherwise buries the failures under a false alarm. */
        if (!this.passed && !this.failed && this.skipped) {
            console.log(c(YELLOW, '  nothing was actually measured — every check skipped'));
        }
        console.log('');
        process.exitCode = this.failed ? 1 : 0;
        return this.failed === 0;
    }
}

/* ---- comparing two byte ranges ------------------------------------------- */

/**
 * Where two buffers differ, as something a report line can carry.
 * Returns { equal, differing, first, total, sample } — `sample` is the first
 * few disagreements as `offset: ours != theirs`, which is usually enough to
 * recognise a whole-buffer offset error from a handful of wrong texels.
 */
export function diffBytes(a, b, { sampleCount = 4 } = {}) {
    const n = Math.min(a.length, b.length);
    let differing = 0, first = -1;
    const sample = [];
    for (let i = 0; i < n; i++) {
        if (a[i] === b[i]) continue;
        if (first < 0) first = i;
        differing++;
        if (sample.length < sampleCount) {
            sample.push(`0x${i.toString(16)}: ${hex2(a[i])}!=${hex2(b[i])}`);
        }
    }
    return {
        equal: differing === 0 && a.length === b.length,
        differing, first, total: n, sample,
        lengthMismatch: a.length !== b.length ? `${a.length} vs ${b.length}` : null,
    };
}

const hex2 = (v) => v.toString(16).padStart(2, '0');

/** `12 of 1048576 bytes differ, first at 0x1a000` — the detail for a diff row. */
export function diffDetail(d) {
    if (d.lengthMismatch) return `length ${d.lengthMismatch}`;
    if (d.equal) return `${d.total} bytes identical`;
    const pct = ((d.differing / d.total) * 100).toFixed(2);
    return `${d.differing} of ${d.total} bytes differ (${pct}%), first at 0x${d.first.toString(16)}` +
           (d.sample.length ? ` [${d.sample.join(', ')}]` : '');
}
