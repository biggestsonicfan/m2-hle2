/*
 * watch-var.mjs — who writes this address, and what do they write?
 *
 * The instrument that replaces "read the disassembly and guess". A watchpoint
 * on the bus stops the i960 on every access to a range and reports the value
 * and the IP that wrote it, so a variable whose owner is unknown can be traced
 * back to the routine that owns it in about a minute.
 *
 * This is how a driving problem gets diagnosed rather than worked around. When
 * pinning a variable does not take, the question is always the same — is the
 * game writing over it, is it reading somewhere else, or is the code that reads
 * it simply not running? — and the three answers look identical from outside.
 * Here they look completely different: writes from a known routine, no writes
 * at all, or writes with values nothing asked for.
 *
 *   node tools/watch-var.mjs 0x500064               # stage_num, 20 hits
 *   node tools/watch-var.mjs 0x500064 --hits 60
 *   node tools/watch-var.mjs 0x50A020 --size 2 --type rw
 *
 * Each hit is one stop and resume, so this is slow by construction — a variable
 * written every frame will not be traced at anything like 60 Hz. That is fine
 * for finding an owner and wrong for anything that needs the game to run at
 * speed; for those, capture instead.
 */
import { M2Hle } from './lib/m2hle.mjs';
import { findRom } from './lib/rom.mjs';
import { parseArgs } from './lib/args.mjs';

const args = parseArgs(['size', 'hits', 'type', 'port', 'timeout']);
const addrArg = args.positional[0];
if (!addrArg) {
    console.error('usage: node tools/watch-var.mjs <addr> [--size N] [--hits N] [--type w|r|rw]');
    process.exit(2);
}
const addr = Number(addrArg.startsWith('0x') ? addrArg : '0x' + addrArg);
const size = args.num('size', 1);
const hits = args.num('hits', 20);
const type = args.str('type', 'w');
const port = args.num('port', 7172);
const timeout = args.num('timeout', 20000);
const attach = args.bool('attach');

const emu = attach
    ? await M2Hle.attach({ port })
    : await M2Hle.launch({ rom: findRom().primary, port });

try {
    await emu.waitUntilRunning();
    await emu.rpc('clear_all_watchpoints');
    await emu.rpc('set_watchpoint', {
        addr: '0x' + addr.toString(16), size, type, label: 'watch-var',
    });
    console.log(`watching [0x${addr.toString(16)}, +${size}) for '${type}', ${hits} hits\n`);

    /* Group by writing IP: what a trace is usually for is the owner, not the
     * individual writes, and one routine writing 200 times is one line. */
    const byIp = new Map();
    let caught = 0, silent = 0;

    for (let i = 0; i < hits; i++) {
        await emu.run();
        const r = await emu.waitForStop(timeout);
        if (r.reason !== 'watchpoint') {
            silent++;
            if (r.reason === 'timeout') {
                console.log(`  no access in ${timeout} ms — nothing is touching it`);
                break;
            }
            console.log(`  stopped for '${r.reason}' at ${r.ip}, not the watchpoint`);
            continue;
        }
        caught++;
        const key = `${r.wp_ip} ${r.wp_write ? 'W' : 'R'}`;
        const e = byIp.get(key) ?? { n: 0, values: new Set(), addrs: new Set() };
        e.n++;
        e.values.add(r.wp_val);
        e.addrs.add(r.wp_addr);
        byIp.set(key, e);
    }

    await emu.rpc('clear_all_watchpoints');

    console.log(`\n${caught} accesses caught` + (silent ? `, ${silent} other stops` : ''));
    if (!caught) {
        console.log('Nothing accessed it. If a value written here is not taking effect, ' +
                    'the code that would read it is not running.');
    }
    for (const [key, e] of [...byIp].sort((a, b) => b[1].n - a[1].n)) {
        const [ip, rw] = key.split(' ');
        const vals = [...e.values].slice(0, 6).join(', ');
        console.log(`  ip=${ip} ${rw}  ${e.n}x  addrs {${[...e.addrs].join(', ')}}  ` +
                    `values {${vals}${e.values.size > 6 ? ', …' : ''}}`);
    }
} finally {
    await emu.close({ kill: !attach });
}
