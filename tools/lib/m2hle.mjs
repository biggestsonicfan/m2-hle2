/*
 * m2hle.mjs — drive a running emulator over its MCP bridge.
 *
 * This is the half of the toolkit that replaces MAME. The explorer's own checks
 * take their ground truth from a MAME session driven by a Lua script and a
 * Python bridge; here the same measurements come out of m2hle.exe, which speaks
 * newline-delimited JSON on 127.0.0.1:7172 when launched with --mcp.
 *
 * Three things the MAME half needed a Lua script inside the emulator for, and
 * where they went:
 *
 *   pacing by game frame   -> wait_frames, on the emu thread's own frame clock
 *   bulk capture to a file -> dump_memory_file, which writes from inside the
 *                             emulator rather than moving megabytes as hex
 *   driving the front end  -> set_input, straight at the I/O port bitmask
 *
 * One request is in flight at a time and replies are matched in order, which is
 * what the bridge itself does: it accepts one client and answers one line per
 * line received. Do not call anything else while a wait_for_stop or wait_frames
 * is outstanding — the bridge is blocked inside it and the reply queue will
 * pair your answers up with the wrong requests.
 */
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { REPO } from './noclip.mjs';

export const DEFAULT_PORT = 7172;

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export const hex = (v) =>
    typeof v === 'string' ? v : '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

/** Where a Release build puts the emulator; the most recently built one wins. */
export function findExe() {
    if (process.env.M2_EXE) return process.env.M2_EXE;
    const candidates = ['build_vs22/Release/m2hle.exe', 'build/Release/m2hle.exe',
                        'build/m2hle', 'build_vs22/m2hle'];
    let best = null, bestTime = -1;
    for (const c of candidates) {
        const p = path.join(REPO, c);
        if (!fs.existsSync(p)) continue;
        const t = fs.statSync(p).mtimeMs;
        if (t > bestTime) { best = p; bestTime = t; }
    }
    if (!best) throw new Error('no m2hle build found — build it, or set $M2_EXE');
    return best;
}

export class M2Hle {
    constructor(sock, { child = null, port = DEFAULT_PORT } = {}) {
        this.sock = sock;
        this.child = child;
        this.port = port;
        this.queue = [];
        this.buf = '';
        this.closed = false;
        sock.on('data', (d) => this.onData(d));
        /* A socket error with no listener is an uncaught exception in Node, and
         * the emulator going away mid-command is a normal thing to survive: it
         * has a window a user can close, and a crash there should be reported
         * as one rather than as a stack trace out of the net module. */
        sock.on('error', (e) => { this.lastError = e; });
        sock.on('close', () => {
            this.closed = true;
            const why = new Error(
                'the emulator went away mid-command' +
                (this.lastError ? ` (${this.lastError.code ?? this.lastError.message})` : '') +
                ' — its window may have been closed, or it crashed');
            while (this.queue.length) this.queue.shift()(why);
        });
    }

    onData(d) {
        this.buf += d.toString('latin1');
        for (let i; (i = this.buf.indexOf('\n')) >= 0; ) {
            const line = this.buf.slice(0, i);
            this.buf = this.buf.slice(i + 1);
            const cb = this.queue.shift();
            if (!cb) continue;
            try { cb(null, JSON.parse(line)); } catch (e) { cb(e); }
        }
    }

    /** One command. Rejects if the bridge answers ok:false, unless allowFail. */
    rpc(cmd, extra = {}, { allowFail = false } = {}) {
        if (this.closed) return Promise.reject(new Error('bridge closed'));
        return new Promise((res, rej) => {
            this.queue.push((err, v) => {
                if (err) return rej(err);
                if (!allowFail && v && v.ok === false) {
                    return rej(new Error(cmd + ': ' + (v.error ?? 'failed')));
                }
                res(v);
            });
            this.sock.write(JSON.stringify({ cmd, ...extra }) + '\n');
        });
    }

    /* ---- connecting ------------------------------------------------------ */

    /** Attach to an emulator that is already listening. */
    static async attach({ port = DEFAULT_PORT, timeoutMs = 20000 } = {}) {
        const deadline = Date.now() + timeoutMs;
        for (;;) {
            try {
                const sock = await new Promise((res, rej) => {
                    const s = net.createConnection({ host: '127.0.0.1', port });
                    s.setNoDelay(true);
                    s.once('connect', () => res(s));
                    s.once('error', rej);
                });
                return new M2Hle(sock, { port });
            } catch {
                if (Date.now() > deadline) {
                    throw new Error('no MCP bridge on :' + port +
                                    ' — start the emulator with --mcp');
                }
                await sleep(250);
            }
        }
    }

    /**
     * Launch an emulator and attach to it. `--run` starts it executing at once;
     * a driver that wants to set breakpoints first should pass run:false.
     */
    static async launch({ rom, port = DEFAULT_PORT, run = true, exe = null,
                          quiet = true, timeoutMs = 30000 } = {}) {
        if (!rom) throw new Error('launch needs a rom path');
        const bin = exe ?? findExe();
        const args = ['--mcp', '--mcp-port', String(port), '--rom', path.resolve(rom)];
        if (run) args.push('--run');
        /* Run it beside the ROM: a split set needs schamp.zip found next to
         * sfight.zip, the same rule the explorer's loader states. */
        const child = spawn(bin, args, {
            cwd: path.dirname(path.resolve(rom)),
            stdio: quiet ? 'ignore' : 'inherit',
        });
        let spawnErr = null;
        child.on('error', (e) => { spawnErr = e; });
        try {
            const conn = await M2Hle.attach({ port, timeoutMs });
            conn.child = child;
            return conn;
        } catch (e) {
            throw spawnErr ?? e;
        }
    }

    /* ---- state ----------------------------------------------------------- */

    status()    { return this.rpc('get_status'); }
    registers() { return this.rpc('get_registers'); }
    run()       { return this.rpc('emu_run'); }
    stop()      { return this.rpc('emu_stop'); }
    step(n = 1) { return this.rpc('emu_step', { count: n }); }

    setInput(held) { return this.rpc('set_input', { held: hex(held) }); }

    setBreakpoint(addr, label = '') { return this.rpc('set_breakpoint', { addr: hex(addr), label }); }
    clearBreakpoint(addr)           { return this.rpc('clear_breakpoint', { addr: hex(addr) }); }
    clearAllBreakpoints()           { return this.rpc('clear_all_breakpoints'); }

    waitForStop(timeoutMs = 30000) { return this.rpc('wait_for_stop', { timeout_ms: timeoutMs }); }

    /**
     * Block until the game has advanced `count` frames. The reply's `reached`
     * is false if the game stalled or stopped instead — a driver should check
     * it rather than assume the frames happened.
     */
    waitFrames(count = 1, timeoutMs = 30000) {
        return this.rpc('wait_frames', { count, timeout_ms: timeoutMs });
    }

    /**
     * Wait for the ROM regions to be filled.
     *
     * Not the same as waiting for a profile: the profile resolves from the
     * set's CRC32s while the regions are still being assembled, so a tool that
     * treats "profile is sfight" as "the data is there" can read a table of
     * zeros. That does not fail — it decodes every model as empty, which reads
     * as a total disagreement rather than as a race.
     */
    async waitForRom({ timeoutMs = 60000 } = {}) {
        const deadline = Date.now() + timeoutMs;
        for (;;) {
            const st = await this.status();
            if (st.rom_loaded) return st;
            if (st.rom_loaded === undefined) {
                throw new Error('this emulator build has no rom_loaded in get_status — rebuild it');
            }
            if (Date.now() > deadline) {
                throw new Error('no ROM loaded after ' + (timeoutMs / 1000) + 's — check the path');
            }
            await sleep(200);
        }
    }

    /**
     * Wait for the emulator to be executing and actually advancing frames.
     *
     * The bridge accepts a client as soon as it is listening, which is well
     * before a 17 MB ROM set has been read and --run has taken effect. Anything
     * that measures the running game has to wait for this first, or it measures
     * a machine that has not started: a status read at connect time reports
     * ip=0 and frames=0, which looks exactly like a game that booted and hung.
     */
    async waitUntilRunning({ timeoutMs = 60000, frames = 2 } = {}) {
        const deadline = Date.now() + timeoutMs;
        let first = null;
        for (;;) {
            const st = await this.status();
            if (st.running) {
                if (first === null) first = st.frames;
                if (st.frames - first >= frames) return st;
            } else {
                first = null;
            }
            if (Date.now() > deadline) {
                throw new Error('emulator never started running (ip=' + st.ip +
                                ', frames=' + st.frames + ') — check the ROM path');
            }
            await sleep(200);
        }
    }

    /* ---- memory ---------------------------------------------------------- */

    /** Read a range as bytes. Chunked at the bridge's own 4096-byte reply cap. */
    async readMemory(addr, size, { onProgress = null } = {}) {
        const out = Buffer.alloc(size);
        const CHUNK = 4096;
        for (let off = 0; off < size; off += CHUNK) {
            const n = Math.min(CHUNK, size - off);
            const r = await this.rpc('read_memory', { addr: hex(addr + off), size: n });
            Buffer.from(r.data, 'hex').copy(out, off);
            if (onProgress) onProgress(off + n, size);
        }
        return out;
    }

    /**
     * Copy a range to a file from inside the emulator, then read it back here.
     *
     * A megabyte of texture RAM is 256 round trips through readMemory and twice
     * that again in hex on the wire; this is one request. It also copies under
     * the emu mutex, so the range is one consistent snapshot rather than a run
     * of reads the i960 wrote through the middle of — which matters for
     * anything the game is still filling.
     */
    async dumpRegion(addr, size, file) {
        const abs = path.resolve(file);
        fs.mkdirSync(path.dirname(abs), { recursive: true });
        const r = await this.rpc('dump_memory_file', { addr: hex(addr), size, path: abs });
        if (r.bytes !== size) throw new Error('dump_memory_file wrote ' + r.bytes + ' of ' + size);
        return { path: abs, bytes: new Uint8Array(fs.readFileSync(abs)), nonzero: r.nonzero };
    }

    writeMemory(addr, bytes) {
        const data = Buffer.from(bytes).toString('hex').toUpperCase();
        return this.rpc('write_memory', { addr: hex(addr), data });
    }

    /* ---- diagnostics ----------------------------------------------------- */

    geoCaptures() { return this.rpc('get_geo_captures'); }
    geoStream()   { return this.rpc('dump_geo_stream'); }
    bones()       { return this.rpc('dump_bones'); }
    copDiag()     { return this.rpc('get_cop_diagnostics'); }

    /* ---- teardown -------------------------------------------------------- */

    async close({ kill = true } = {}) {
        this.closed = true;
        try { this.sock.destroy(); } catch { /* already gone */ }
        if (kill && this.child) {
            this.child.kill();
            /* Give it a moment to unwind before the process exits under it. */
            await sleep(200);
        }
    }
}
