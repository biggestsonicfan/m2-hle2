/*
 * cop-replay.mjs — the coprocessor's current matrix, replayed from the words the
 * i960 wrote to its FIFO.
 *
 * Every command that writes the current matrix is applied as the firmware does
 * it (stf-sharc cpres1.asm), including the three matrix banks the stream fills
 * and reads back: the unit-matrix cache (0x35/0x36/0x37, by player and slot),
 * the inner bank (0x43/0x44/0x45/0x46, by index) and the global slots
 * (0x67/0x68). Two things in the listing's own comments read the wrong way and
 * are followed here as the code runs, not as the comment says:
 *
 *   _L201EA (0x0B, 0x37, 0x45): each new column is rot x B's column and
 *       T' = rot x B_T + T — current * M in row-major terms, a post-multiply.
 *   _L2021F (0x46, 0x47): each column is B_rot x the current column —
 *       M * current.
 *
 * The replay cannot know a matrix the stream does not carry: a bank entry it
 * never saw stored, a shadow projection built off a floor height in DM, an IK
 * solve. Those mark the matrix `tainted` until a pop or a load clears it, and a
 * caller that is comparing numbers leaves a tainted draw out rather than guess.
 *
 * `float32: true` is the chip's arithmetic (fround at each step, sine and cosine
 * out of the coprocessor ROM) — what the emulator's own words should equal.
 * The default is double precision with libm trig, which is what an explorer op
 * list compares against without accumulating a rounding difference of its own.
 */

const f32buf = new DataView(new ArrayBuffer(4));
export function f32(w) { f32buf.setUint32(0, w >>> 0, true); return f32buf.getFloat32(0, true); }

/** The i960 encodes opcode n as (n<<23)|(n<<8)|n, so a command word names itself. */
export const isCmd = (w) => {
    const op = w & 0xff;
    return op !== 0 && w === ((((op << 23) >>> 0) | (op << 8) | op) >>> 0);
};

export const COPRO_FIFO = [0x884000, 0x888000];
export const GEO_PROGRAM = [0x804000, 0x808000];

/* Commands that leave the current matrix at something the stream does not say. */
const OPAQUE = new Set([0x41, 0x42, 0x4a, 0x54, 0x55, 0x62, 0x69, 0x6b, 0x79, 0x7a, 0x7c, 0x7e, 0x83, 0x84]);

const s16 = (w) => (w << 16) >> 16;

export class CopReplay {
    /**
     * @param {object} o
     * @param {boolean} [o.float32]  the chip's arithmetic
     * @param {DataView} [o.copro]   the coprocessor ROM (rom.coproView), for float32 trig
     */
    constructor({ float32 = false, copro = null } = {}) {
        this.fr = float32 ? Math.fround : (x) => x;
        if (float32 && copro) {
            this.sin = (w) => copro.getFloat32((0x10000 + s16(w)) * 4, true);
            this.cos = (w) => copro.getFloat32((0x30000 + s16(w)) * 4, true);
        } else {
            this.sin = (w) => this.fr(Math.sin(s16(w) * Math.PI / 32768));
            this.cos = (w) => this.fr(Math.cos(s16(w) * Math.PI / 32768));
        }
        this.R = [1, 0, 0, 0, 1, 0, 0, 0, 1];
        this.T = [0, 0, 0];
        this.tainted = true;         /* nothing is known until a base or a load */
        this.base3x3 = false;        /* the 3x3 was reset since the last load */
        this.stack = [];
        this.banks = { unit: new Map(), inner: new Map(), glb: new Map() };
    }

    state() { return { R: this.R.slice(), T: this.T.slice(), tainted: this.tainted, base3x3: this.base3x3 }; }
    restore(s) { this.R = s.R.slice(); this.T = s.T.slice(); this.tainted = s.tainted; this.base3x3 = s.base3x3; }

    /** The current matrix, row-major. */
    matrix(R = this.R, T = this.T) {
        return [R[0], R[3], R[6], T[0], R[1], R[4], R[7], T[1], R[2], R[5], R[8], T[2], 0, 0, 0, 1];
    }
    /** The matrix at the bottom of the stack — the view, for a stage draw. */
    base() { return this.stack.length ? this.matrix(this.stack[0].R, this.stack[0].T) : this.matrix(); }
    /** The 12 words as the slot holds them: col0, col1, col2, T. */
    slot() { return [...this.R, ...this.T]; }

    setRowMajor(m) {
        const fr = this.fr;
        this.R = [m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]].map(fr);
        this.T = [m[3], m[7], m[11]].map(fr);
    }

    #col(c) { return this.R.slice(c * 3, c * 3 + 3); }
    #setCol(c, v) { this.R[c * 3] = v[0]; this.R[c * 3 + 1] = v[1]; this.R[c * 3 + 2] = v[2]; }
    /* Post-multiply by a rotation in the plane of columns i and j (cpres1 PM 0x201AA..0x201D4). */
    #rot(w, i, j, sign) {
        const fr = this.fr, c = this.cos(w), s = this.sin(w);
        const a = this.#col(i), b = this.#col(j);
        this.#setCol(i, a.map((v, k) => fr(fr(c * v) + fr(sign * s * b[k]))));
        this.#setCol(j, a.map((v, k) => fr(fr(-sign * s * v) + fr(c * b[k]))));
    }

    /** Apply one command (opcode byte, argument words). */
    apply(op, a) {
        const fr = this.fr;
        const cols12 = () => this.matrix(a.slice(0, 9).map(f32), a.slice(9, 12).map(f32));
        switch (op) {
            case 0x01: this.stack.push(this.state()); break;                         /* Fn_push_matrix */
            case 0x02: if (this.stack.length) this.restore(this.stack.pop()); break;  /* Fn_pop_matrix */
            case 0x03:                                                                /* Fn_base_matrix */
                this.R = [1, 0, 0, 0, 1, 0, 0, 0, 1]; this.T = [0, 0, 0];
                this.tainted = false; this.base3x3 = false; break;
            case 0x04:                                                                /* Fn_load_matrix */
                this.R = a.slice(0, 9).map(f32); this.T = a.slice(9, 12).map(f32);
                this.tainted = false; this.base3x3 = false; break;
            case 0x06:                                                                /* Fn_trans */
                for (let c = 0; c < 3; c++) {
                    const v = f32(a[c]);
                    for (let w = 0; w < 3; w++) this.T[w] = fr(this.T[w] + fr(v * this.R[c * 3 + w]));
                }
                break;
            case 0x07:                                                                /* Fn_scale */
                for (let c = 0; c < 3; c++) for (let r = 0; r < 3; r++) this.R[c * 3 + r] = fr(this.R[c * 3 + r] * f32(a[c]));
                break;
            case 0x08: this.#rot(a[0], 1, 2, -1); break;                              /* Fn_x_rot */
            case 0x09: this.#rot(a[0], 0, 2, 1); break;                               /* Fn_y_rot */
            case 0x0a: this.#rot(a[0], 0, 1, -1); break;                              /* Fn_z_rot */
            case 0x0b: this.setRowMajor(mul4(this.matrix(), cols12())); break;        /* Fn_mul_matrix */
            case 0x0c: { const m = inv4(this.matrix()); if (m) this.setRowMajor(m); break; } /* Fn_inv_matrix */
            case 0x0d: this.T = [0, 0, 0]; break;                                     /* Fn_base_point */
            case 0x0e: this.T = a.slice(0, 3).map(f32); break;                        /* Fn_load_point */
            case 0x10: this.R = [1, 0, 0, 0, 1, 0, 0, 0, 1]; this.base3x3 = true; break; /* Fn_base_3x3 */
            case 0x11: this.R = a.slice(0, 9).map(f32); break;                        /* Fn_load_3x3 */
            case 0x3f: this.#rot(a[0], 0, 1, -1); this.#rot(a[1], 0, 2, 1); this.#rot(a[2], 1, 2, -1); break; /* Fn_zyx_rot */
            case 0x47: this.setRowMajor(mul4(cols12(), this.matrix())); break;        /* Fn_mul_matrix_rev */
            case 0x35: case 0x43: case 0x67: {                                        /* store into a bank */
                const bank = op === 0x35 ? this.banks.unit : op === 0x43 ? this.banks.inner : this.banks.glb;
                bank.set(a.join(','), this.state());
                break;
            }
            case 0x36: case 0x44: case 0x68: {                                        /* load from a bank */
                const bank = op === 0x36 ? this.banks.unit : op === 0x44 ? this.banks.inner : this.banks.glb;
                const e = bank.get(a.join(','));
                if (e) { this.restore(e); this.base3x3 = false; } else this.tainted = true;
                break;
            }
            case 0x37: case 0x45: case 0x46: {                                        /* compose with a bank entry */
                const e = (op === 0x37 ? this.banks.unit : this.banks.inner).get(a.join(','));
                if (!e) { this.tainted = true; break; }
                const M = this.matrix(e.R, e.T), C = this.matrix();
                this.setRowMajor(op === 0x46 ? mul4(M, C) : mul4(C, M));
                this.tainted = this.tainted || e.tainted;
                break;
            }
            case 0x73:                                                                /* Fn_kage_mat */
                /* Pushes and pops round its own work; inner slots 0..2 take shadow
                 * projections off the floor height in DM. */
                for (const k of ['0', '1', '2']) this.banks.inner.set(k, { ...this.state(), tainted: true });
                break;
            case 0x74:                                                                /* Fn_kage_poly */
                /* Leaves a part's shadow matrix pushed for the draw; the i960 pops. */
                this.stack.push(this.state());
                this.tainted = true;
                break;
            default:
                if (OPAQUE.has(op)) this.tainted = true;
        }
    }
}

function mul4(a, b) {
    const o = new Array(16);
    for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) {
        let s = 0;
        for (let k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
        o[r * 4 + c] = s;
    }
    return o;
}
function inv4(m) {
    const a = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]];
    const d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (!(Math.abs(d) > 1e-20)) return null;
    const c = [(a[4] * a[8] - a[5] * a[7]) / d, -(a[1] * a[8] - a[2] * a[7]) / d, (a[1] * a[5] - a[2] * a[4]) / d,
        -(a[3] * a[8] - a[5] * a[6]) / d, (a[0] * a[8] - a[2] * a[6]) / d, -(a[0] * a[5] - a[2] * a[3]) / d,
        (a[3] * a[7] - a[4] * a[6]) / d, -(a[0] * a[7] - a[1] * a[6]) / d, (a[0] * a[4] - a[1] * a[3]) / d];
    const t = [m[3], m[7], m[11]];
    return [c[0], c[1], c[2], -(c[0] * t[0] + c[1] * t[1] + c[2] * t[2]), c[3], c[4], c[5], -(c[3] * t[0] + c[4] * t[1] + c[5] * t[2]),
        c[6], c[7], c[8], -(c[6] * t[0] + c[7] * t[1] + c[8] * t[2]), 0, 0, 0, 1];
}

/**
 * Walk a capture's records (offset, word) in write order, frame by frame, with
 * one replay carried across the marks. Yields, per frame, every object the i960
 * laid down: through Fn_put_poly (with the list offset it named) or handed
 * straight to the geometry processor (a mesh pointer with its uv pointer two
 * words ahead, drawn at the matrix standing then).
 *
 * @param {Buffer} bin     <capture>.bin: (offset, word) pairs
 * @param {number[][]} marks  <capture>.json marks: [frame, word index, ...]
 * @param {object} o
 * @param {CopReplay} o.replay
 * @param {Map} o.byEntry  `${tpa}/${tha}/${oba}` -> model
 * @param {Map} o.byMesh   oba -> { model, uvPtr, matPtr }
 */
export function* replayDraws(bin, marks, { replay, byEntry, byMesh }) {
    let pending = null, pendingFrame = 0;
    const geo = [];
    for (let fi = 0; fi + 1 < marks.length; fi++) {
        const draws = [];
        const flush = () => {
            if (!pending) return;
            const [op, args] = [pending[0] & 0xff, pending.slice(1)];
            if (op === 0x78 && args.length >= 5) {
                draws.push({
                    via: 'copro', frame: pendingFrame, model: byEntry.get(`${args[2]}/${args[3]}/${args[4]}`) ?? -1,
                    listOffset: args[0] >>> 0, oba: args[4],
                    m: replay.matrix(), base: replay.base(), slot: replay.slot(),
                    tainted: replay.tainted, base3x3: replay.base3x3,
                });
            }
            replay.apply(op, args);
            pending = null;
        };
        for (let k = marks[fi][1]; k < marks[fi + 1][1]; k++) {
            const o = bin.readUInt32LE(k * 8), w = bin.readUInt32LE(k * 8 + 4);
            if (o >= GEO_PROGRAM[0] && o < GEO_PROGRAM[1]) {
                geo.push(w >>> 0);
                if (geo.length > 3) geo.shift();
                const hit = geo.length === 3 ? byMesh.get(geo[2]) : null;
                /* (tpa, tha, oba): the model's own texture pointers, or its header
                 * pointer with texture points uploaded to geometrizer RAM (bit 23)
                 * — how the aurora hands over its scrolled points. */
                if (hit && (geo[0] === hit.uvPtr || (geo[1] === hit.matPtr && (geo[0] & 0x800000)))) {
                    draws.push({ via: 'geo', frame: fi, model: hit.model, tpa: geo[0], m: replay.matrix(), base: replay.base(),
                                 tainted: replay.tainted, base3x3: replay.base3x3 });
                }
                continue;
            }
            if (o < COPRO_FIFO[0] || o >= COPRO_FIFO[1]) continue;
            if (isCmd(w)) { flush(); pending = [w]; pendingFrame = fi; } else if (pending) pending.push(w);
        }
        yield draws;
    }
}
