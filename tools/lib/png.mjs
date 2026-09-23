/*
 * png.mjs — just enough PNG for the graders: read MAME's snapshots (8-bit RGB or
 * RGBA, non-interlaced, any filter) and write RGB pictures back out, with
 * node's own zlib and nothing else.
 */
import fs from 'node:fs';
import zlib from 'node:zlib';

const SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/** Decode a PNG file to { width, height, rgb } with rgb 3 bytes a pixel. */
export function readPng(file) {
    const b = fs.readFileSync(file);
    if (!b.subarray(0, 8).equals(SIG)) throw new Error(`${file}: not a PNG`);
    let width = 0, height = 0, depth = 0, type = 0, interlace = 0;
    const idat = [];
    for (let o = 8; o < b.length;) {
        const len = b.readUInt32BE(o), kind = b.toString('latin1', o + 4, o + 8), data = b.subarray(o + 8, o + 8 + len);
        if (kind === 'IHDR') {
            width = data.readUInt32BE(0); height = data.readUInt32BE(4);
            depth = data[8]; type = data[9]; interlace = data[12];
        } else if (kind === 'IDAT') idat.push(data);
        else if (kind === 'IEND') break;
        o += 12 + len;
    }
    if (depth !== 8 || (type !== 2 && type !== 6) || interlace)
        throw new Error(`${file}: only 8-bit RGB/RGBA non-interlaced PNGs (depth ${depth}, type ${type}, interlace ${interlace})`);
    const bpp = type === 6 ? 4 : 3, stride = width * bpp;
    const raw = zlib.inflateSync(Buffer.concat(idat));
    const cur = Buffer.alloc(stride), prev = Buffer.alloc(stride);
    const rgb = Buffer.alloc(width * height * 3);
    for (let y = 0; y < height; y++) {
        const f = raw[y * (stride + 1)], row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
        for (let x = 0; x < stride; x++) {
            const a = x >= bpp ? cur[x - bpp] : 0, up = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
            let v = row[x];
            if (f === 1) v += a;
            else if (f === 2) v += up;
            else if (f === 3) v += (a + up) >> 1;
            else if (f === 4) {
                const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
            }
            cur[x] = v & 0xff;
        }
        for (let x = 0; x < width; x++) cur.copy(rgb, (y * width + x) * 3, x * bpp, x * bpp + 3);
        cur.copy(prev);
    }
    return { width, height, rgb };
}

const CRC = new Int32Array(256).map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c;
});
function crc32(buf) {
    let c = -1;
    for (const x of buf) c = CRC[(c ^ x) & 0xff] ^ (c >>> 8);
    return (c ^ -1) >>> 0;
}
function chunk(kind, data) {
    const head = Buffer.alloc(8);
    head.writeUInt32BE(data.length, 0);
    head.write(kind, 4, 'latin1');
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])));
    return Buffer.concat([head, data, crc]);
}

/** Encode { width, height, rgb } (3 bytes a pixel) to a PNG file. */
export function writePng(file, { width, height, rgb }) {
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8; ihdr[9] = 2;
    const raw = Buffer.alloc(height * (width * 3 + 1));
    for (let y = 0; y < height; y++) rgb.copy(raw, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
    fs.writeFileSync(file, Buffer.concat([SIG, chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}
