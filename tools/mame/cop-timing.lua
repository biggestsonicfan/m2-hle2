-- Time every COP command under MAME, for tools/mame/cop_timing.py: both sides
-- of the two COP FIFOs, stamped with the board clock (i960 cycles, 25 MHz), so
-- the script can say how long the SHARC spends on each command and how long
-- the i960 sits waiting for it.
--
-- One 17-byte record "<B d I4 I4" (kind, i960 cycles, word, extra) per event:
--   1  the SHARC read a command word (PM 0x20141); extra = bufferram reads << 16
--      | bufferram writes the SHARC made since the previous command word
--   2  the SHARC read an argument word
--   3  the SHARC wrote a reply word
--   4  the i960 wrote a word to the COP FIFO (0x884000); extra = the store's
--      address (PIP)
--   5  the i960 read the reply FIFO (0x884000); extra = the load's address.
--      A read that finds it empty returns 0 and MAME runs the load again
--      (i960_stall), so a wait shows as a run of reads from one address
--   6  a frame edge; word = the game's frame counter (0x500020), extra =
--      also_sub_mode (0x500031) << 8 | screen frame & 0xff
--
--   SDL_VIDEODRIVER=dummy shared sfight -rompath <dir> -nodrc -video none \
--     -sound none -nothrottle -skip_gameinfo -autoboot_script cop-timing.lua
-- with $COP_TIMING_OUT (file, default /dev/shm/cop-timing.bin) and
-- $COP_TIMING_FROM / $COP_TIMING_TO (game frame counter, default 0x900-0xD00).
local out    = os.getenv("COP_TIMING_OUT") or "/dev/shm/cop-timing.bin"
local from_f = tonumber(os.getenv("COP_TIMING_FROM") or "2304")
local to_f   = tonumber(os.getenv("COP_TIMING_TO") or "3328")
local CLK = 25000000.0
local pack = string.pack
local state = "armed"
local taps, f
local buf, nb = {}, 0
local bufr, bufw = 0, 0

local function rec(kind, word, extra)
    nb = nb + 1
    buf[nb] = pack("<BdI4I4", kind, manager.machine.time:as_double() * CLK, word & 0xffffffff, extra)
    if nb == 8192 then f:write(table.concat(buf)); buf = {}; nb = 0 end
end

local function start()
    f = assert(io.open(out, "wb"))
    local i960 = manager.machine.devices[":maincpu"].spaces["program"]
    local cop = manager.machine.devices[":copro_adsp"].spaces["data"]
    local pcst = manager.machine.devices[":copro_adsp"].state["PC"]
    local ipst = manager.machine.devices[":maincpu"].state["pip"]
    taps = {
        cop:install_read_tap(0x0400000, 0x0bfffff, "ctin", function(offset, data, mask)
            if pcst.value == 0x20141 then
                rec(1, data, (bufr << 16) | (bufw & 0xffff)); bufr, bufw = 0, 0
            else
                rec(2, data, 0)
            end
            return data end),
        cop:install_write_tap(0x0c00000, 0x13fffff, "ctout", function(offset, data, mask) rec(3, data, 0) end),
        cop:install_read_tap(0x1400000, 0x1bfffff, "ctbr", function(offset, data, mask) bufr = bufr + 1; return data end),
        cop:install_write_tap(0x1400000, 0x1bfffff, "ctbw", function(offset, data, mask) bufw = bufw + 1 end),
        i960:install_write_tap(0x884000, 0x887fff, "ctw", function(offset, data, mask) rec(4, data, ipst.value) end),
        i960:install_read_tap(0x884000, 0x887fff, "ctr", function(offset, data, mask) rec(5, data, ipst.value); return data end),
    }
    state = "on"
end

local function stop()
    for _, t in ipairs(taps) do t:remove() end
    if nb > 0 then f:write(table.concat(buf)) end
    f:close()
    state = "done"
    print("cop-timing: done")
    manager.machine:exit()
end

local scr, sp
local function tick()
    if state == "done" then return end
    if not sp then
        sp = manager.machine.devices[":maincpu"].spaces["program"]
        scr = manager.machine.screens[":screen"]
    end
    local fc = sp:read_u32(0x500020)
    if state == "armed" and fc >= from_f and fc < to_f then start(); print("cop-timing: on at fc " .. fc) end
    if state == "on" then
        rec(6, fc, (sp:read_u8(0x500031) << 8) | (scr:frame_number() & 0xff))
        if fc >= to_f then stop() end
    end
end
_G.COP_TIMING = emu.add_machine_frame_notifier(function()
    local ok, e = pcall(tick)
    if not ok then print("cop-timing: " .. tostring(e)); state = "done"; manager.machine:exit() end
end)
print("cop-timing.lua loaded")
