#!/usr/bin/env python3
"""
core-options.py -- a fake libretro frontend, just enough to see the option table
the core registers and what it asks to have hidden.

    python tools/libretro/core-options.py build_lr/Release/m2hle_libretro.dll

The RPCN lobby in the libretro core IS that table (src/ui/retro_lobby.h), so the
things a frontend would quietly ignore are worth catching here rather than in
RetroArch's menu:

  * a default_value that is not one of the option's values -- libretro.h says
    "this option will be ignored", and the row simply does not appear;
  * a values array that is not terminated, or holds the same key twice;
  * an option the core never registered, or registered twice;
  * the update-display callback leaving a row visible that has nothing behind
    it (with online play set to RetroArch, none of the RPCN rows belong).

It only calls retro_set_environment, so it needs neither a ROM nor a GL context.
Run it after touching the option table; it exits non-zero on a fault.
"""
import ctypes as C
import os
import sys

MAXV = 128          # RETRO_NUM_CORE_OPTION_VALUES_MAX


class Value(C.Structure):
    _fields_ = [("value", C.c_char_p), ("label", C.c_char_p)]


class Category(C.Structure):
    _fields_ = [("key", C.c_char_p), ("desc", C.c_char_p), ("info", C.c_char_p)]


class Definition(C.Structure):
    _fields_ = [("key", C.c_char_p), ("desc", C.c_char_p), ("desc_categorized", C.c_char_p),
                ("info", C.c_char_p), ("info_categorized", C.c_char_p),
                ("category_key", C.c_char_p), ("values", Value * MAXV),
                ("default_value", C.c_char_p)]


class OptionsV2(C.Structure):
    _fields_ = [("categories", C.POINTER(Category)), ("definitions", C.POINTER(Definition))]


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class Display(C.Structure):
    _fields_ = [("key", C.c_char_p), ("visible", C.c_bool)]


GET_VARIABLE             = 15
SET_CONTROLLER_INFO      = 35
GET_CORE_OPTIONS_VERSION = 52
SET_CORE_OPTIONS_DISPLAY = 55
SET_CORE_OPTIONS_V2      = 67
SET_UPDATE_DISPLAY_CB    = 69

ENV = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)

tables = []      # every option table the core has sent
visible = {}     # key -> what it last asked for
variables = {}   # what this "frontend" currently holds
update_cb = []


def _text(p):
    return p.decode("utf-8", "replace") if p else None


def environment(cmd, data):
    if cmd == GET_CORE_OPTIONS_VERSION:
        C.cast(data, C.POINTER(C.c_uint))[0] = 2
        return True
    if cmd == SET_CORE_OPTIONS_V2:
        opts = C.cast(data, C.POINTER(OptionsV2)).contents
        rows, i = [], 0
        while opts.definitions[i].key:
            d = opts.definitions[i]
            vals, k = [], 0
            while k < MAXV and d.values[k].value:
                vals.append((_text(d.values[k].value), _text(d.values[k].label)))
                k += 1
            rows.append(dict(key=_text(d.key), desc=_text(d.desc),
                             category=_text(d.category_key),
                             default=_text(d.default_value), values=vals))
            i += 1
        tables.append(rows)
        return True
    if cmd == SET_CORE_OPTIONS_DISPLAY:
        d = C.cast(data, C.POINTER(Display)).contents
        visible[_text(d.key)] = d.visible
        return True
    if cmd == GET_VARIABLE:
        v = C.cast(data, C.POINTER(Variable)).contents
        held = variables.get(_text(v.key))
        if held is None:
            return False
        v.value = held.encode()
        return True
    if cmd == SET_UPDATE_DISPLAY_CB:
        if data:
            fn = C.cast(data, C.POINTER(C.c_void_p))[0]
            update_cb.append(C.CFUNCTYPE(C.c_bool)(fn))
        return True
    if cmd == SET_CONTROLLER_INFO:
        return True
    return False


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    path = sys.argv[1]
    if not os.path.exists(path):
        sys.exit("no core at %s" % path)

    callback = ENV(environment)
    core = C.CDLL(path)
    core.retro_set_environment.argtypes = [ENV]
    core.retro_set_environment(callback)

    if not tables:
        sys.exit("FAIL: the core registered no v2 option table")
    rows = tables[-1]
    faults = []
    print("%d options, %d table(s) sent" % (len(rows), len(tables)))
    for r in rows:
        keys = [v[0] for v in r["values"]]
        notes = ""
        if not keys:
            faults.append("%s has no values" % r["key"])
            notes += "   <-- NO VALUES"
        if r["default"] not in keys:
            faults.append("%s: default %r is not one of its values" % (r["key"], r["default"]))
            notes += "   <-- DEFAULT NOT IN VALUES"
        if len(set(keys)) != len(keys):
            faults.append("%s repeats a value key" % r["key"])
            notes += "   <-- DUPLICATE VALUE KEY"
        print("  %-22s cat=%-8s default=%-10s %2d values%s"
              % (r["key"], r["category"], r["default"], len(keys), notes))
        for value, label in r["values"][:4]:
            print("        %-12s %s" % (value, label if label else ""))
        if len(r["values"]) > 4:
            print("        ... %d more" % (len(r["values"]) - 4))

    seen = [r["key"] for r in rows]
    if len(set(seen)) != len(seen):
        faults.append("the table repeats an option key")
    for older in tables[:-1]:
        if len(older) != len(rows):
            faults.append("the option COUNT changed between tables (%d then %d); libretro.h "
                          "forbids it and a frontend will ignore the second"
                          % (len(older), len(rows)))

    # The frontend is about to draw the list and asks what to leave out.
    if not update_cb:
        print("\nno update-display callback registered")
    else:
        for online in ("retroarch", "rpcn"):
            variables["m2hle_online"] = online
            visible.clear()
            update_cb[0]()
            shown = sorted(k for k, on in visible.items() if on)
            gone = sorted(k for k, on in visible.items() if not on)
            print("\nonline play = %s" % online)
            print("  shown:  %s" % (", ".join(shown) if shown else "(none)"))
            print("  hidden: %s" % (", ".join(gone) if gone else "(none)"))
            if online == "retroarch":
                for key in shown:
                    if key.startswith("m2hle_rpcn") or key.startswith("m2hle_room"):
                        faults.append("%s is shown with online play set to RetroArch" % key)

    print()
    for f in faults:
        print("FAIL: %s" % f)
    print("PASS: the option table is well formed" if not faults
          else "FAIL: %d fault(s)" % len(faults))
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main())
