-- Trace the sound 68000 under MAME for tools/mame/m68k_timing_compare.py:
-- between two frames, the instruction order (the debugger's trace, loops kept)
-- and every read of the program ROM stamped with the 68000 clock (a Lua read
-- tap), from which the compare script takes each instruction's opcode fetch.
--
-- The debugger's own per-instruction action ({tracelog ...}) writes nothing
-- under -debugger none, with or without `focus`, so the clock comes from the
-- tap instead. Both files land beside OUT.
--
--   SDL_VIDEODRIVER=dummy m2 sfight -rompath <3 zips> -nodrc -video none -sound none \
--     -nothrottle -skip_gameinfo -debug -debugger none -seconds_to_run 299 \
--     -autoboot_script tools/mame/m68k-trace.lua
-- with $M68K_TRACE_OUT (prefix, default /tmp/m68k), $M68K_TRACE_FROM and
-- $M68K_TRACE_TO (frames, default 1700-1800: about 29.5-31.3 s, 1.1 M
-- instructions, 80 MB).
local out    = os.getenv("M68K_TRACE_OUT") or "/tmp/m68k"
local from_f = tonumber(os.getenv("M68K_TRACE_FROM") or "1700")
local to_f   = tonumber(os.getenv("M68K_TRACE_TO") or "1800")
local CLK = 11289600.0
local started, stopped = false, false
local scr, tap, f
local buf, nb = {}, 0
local function tick()
    if not scr then scr = manager.machine.screens[":screen"] end
    local fr = scr:frame_number()
    if not started and fr >= from_f then
        started = true
        f = assert(io.open(out .. ".fetch.bin", "wb"))
        local pack = string.pack
        local snd = manager.machine.devices[":audiocpu"].spaces["program"]
        tap = snd:install_read_tap(0x600000, 0x67ffff, "fetch", function(offset, data, mask)
            local t = math.floor(manager.machine.time:as_double() * CLK + 0.5)
            nb = nb + 1
            buf[nb] = pack("<I4I4", offset, t & 0xffffffff)
            if nb == 8192 then f:write(table.concat(buf)); buf = {}; nb = 0 end
            return data
        end)
        manager.machine.debugger:command('trace ' .. out .. '.tr,audiocpu,noloop')
        print("m68k trace started at frame " .. fr)
    elseif started and not stopped and fr >= to_f then
        stopped = true
        manager.machine.debugger:command('trace off,audiocpu')
        tap:remove()
        if nb > 0 then f:write(table.concat(buf)) end
        f:close()
        print("m68k trace stopped at frame " .. fr)
        manager.machine:exit()
    end
end
_G.M68K_TRACE = emu.add_machine_frame_notifier(tick)   -- a local would be collected
print("m68k-trace.lua loaded")
