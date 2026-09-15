/*
 * matrix.mjs — 4x4 affine matrices in the board's row-major convention, and the
 * explorer's op lists turned into them.
 *
 * The coprocessor keeps its current matrix column-major (three columns, then T);
 * written out row-major, an ang_y from the identity is
 *
 *     [ c 0 -s ]
 *     [ 0 1  0 ]
 *     [ s 0  c ]
 *
 * which is the transpose of the rotation the same angle names right-handed. The
 * explorer's op lists are in its own space: a translate's Z and an ang_z's sense
 * are negated on the way in (display.js, ANGLE_DEG). boardMatrix undoes that, so
 * an explorer part and a board draw can be compared as numbers.
 */

export const I4 = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

export function mul(a, b) {
    const o = new Array(16);
    for (let r = 0; r < 4; r++) {
        for (let c = 0; c < 4; c++) {
            let s = 0;
            for (let k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
    }
    return o;
}

/** Inverse of an affine matrix, or null when its 3x3 is singular. */
export function inv(m) {
    const a = [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]];
    const d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6])
            + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (!(Math.abs(d) > 1e-20)) return null;
    const c = [
        (a[4] * a[8] - a[5] * a[7]) / d, -(a[1] * a[8] - a[2] * a[7]) / d, (a[1] * a[5] - a[2] * a[4]) / d,
        -(a[3] * a[8] - a[5] * a[6]) / d, (a[0] * a[8] - a[2] * a[6]) / d, -(a[0] * a[5] - a[2] * a[3]) / d,
        (a[3] * a[7] - a[4] * a[6]) / d, -(a[0] * a[7] - a[1] * a[6]) / d, (a[0] * a[4] - a[1] * a[3]) / d,
    ];
    const t = [m[3], m[7], m[11]];
    return [
        c[0], c[1], c[2], -(c[0] * t[0] + c[1] * t[1] + c[2] * t[2]),
        c[3], c[4], c[5], -(c[3] * t[0] + c[4] * t[1] + c[5] * t[2]),
        c[6], c[7], c[8], -(c[6] * t[0] + c[7] * t[1] + c[8] * t[2]),
        0, 0, 0, 1,
    ];
}

export const maxdiff = (a, b) => a.reduce((m, v, i) => Math.max(m, Math.abs(v - b[i])), 0);

/** A matrix as a string, for telling "the same matrix" apart from float noise. */
export const matKey = (m) => m.map((v) => (Math.abs(v) < 1e-9 ? 0 : v).toFixed(6)).join(',');

export const scaleM = (x, y, z) => [x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1];
export const transM = (x, y, z) => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1];

export function rotY(deg) {
    const t = deg * Math.PI / 180, c = Math.cos(t), s = Math.sin(t);
    return [c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1];
}
export function rotX(deg) {
    const t = deg * Math.PI / 180, c = Math.cos(t), s = Math.sin(t);
    return [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1];
}
export function rotZ(deg) {
    const t = deg * Math.PI / 180, c = Math.cos(t), s = Math.sin(t);
    return [c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

/* 16-bit angles: an op list's degrees are rounded to the unit the board sends. */
export const ANGLE_UNIT = 360 / 65536;

/** An explorer op list as the board accumulates it. Billboards ('b') are not ops here; split on them first. */
export function boardMatrix(ops) {
    let m = I4();
    for (const [kind, raw] of ops) {
        const v = kind[0] === 'r' ? Math.round(raw / ANGLE_UNIT) * ANGLE_UNIT : raw;
        if (kind === 's') m = mul(m, scaleM(v[0], v[1], v[2]));
        else if (kind === 'r') m = mul(m, rotY(v));
        else if (kind === 'rx') m = mul(m, rotX(v));
        else if (kind === 'rz') m = mul(m, rotZ(-v));
        else if (kind === 't') m = mul(m, transM(v[0], v[1], -v[2]));
        else throw new Error(`boardMatrix: op '${kind}' has no matrix`);
    }
    return m;
}

/** A residual transform described in the terms a display list is written in. */
export function describeResidual(D) {
    const s = [0, 1, 2].map((i) => Math.hypot(D[i], D[4 + i], D[8 + i]));
    const t = [D[3], D[7], D[11]];
    const parts = [];
    if (s.some((v) => Math.abs(v - 1) > 1e-4)) parts.push(`scale ${s.map((v) => v.toFixed(4)).join('/')}`);
    const yaw = Math.atan2(-D[2] / s[2], D[0] / s[0]) * 180 / Math.PI;
    if (Math.abs(yaw) > 1e-3) parts.push(`yaw ${yaw.toFixed(3)}°`);
    if (t.some((v) => Math.abs(v) > 1e-4)) parts.push(`offset ${t.map((v) => v.toFixed(3)).join(',')}`);
    if (!parts.length) parts.push(`rotation off Y (largest element ${maxdiff(D, I4()).toExponential(1)})`);
    return parts.join(', ');
}
