/*
 * args.mjs — the flag parsing every tool here does the same way.
 *
 * Deliberately tiny and dependency-free. A flag that takes a value has to be
 * declared, so a positional argument is never swallowed as one — `--stage 5 out`
 * and `--attach out` both leave `out` where the caller expects it.
 */

/**
 * @param {string[]} valued  names of flags that consume the next argument
 * @param {string[]} [argv]  defaults to the process arguments
 */
export function parseArgs(valued = [], argv = process.argv.slice(2)) {
    const takes = new Set(valued);
    const flags = {}, positional = [];
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (!a.startsWith('--')) { positional.push(a); continue; }
        const eq = a.indexOf('=');
        if (eq > 0) { flags[a.slice(2, eq)] = a.slice(eq + 1); continue; }
        const name = a.slice(2);
        flags[name] = takes.has(name) ? argv[++i] : true;
    }
    return {
        flags,
        positional,
        has: (n) => flags[n] === true || flags[n] !== undefined,
        bool: (n) => flags[n] === true,
        str: (n, d = null) => (flags[n] !== undefined && flags[n] !== true ? String(flags[n]) : d),
        num: (n, d = null) => (flags[n] !== undefined && flags[n] !== true ? Number(flags[n]) : d),
    };
}
