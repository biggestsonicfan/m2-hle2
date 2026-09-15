-- Capture the sound board's side of sfight off MAME, from power-on, for grading
-- m2-hle2's 68000 + SCSP against it. Every record is four u32 words:
--   [tag << 24 | offset, data, t, pc]
-- with t in 68000 clock cycles since power-on (11.2896 MHz = 44100 * 256, so
-- t >> 8 is the SCSP output sample) and pc the 68000's PC (MAME reports it
-- prefetch-biased, +2) or 0 for i960-side records.
--   tag 1  MIDI byte the i960 wrote to the sound UART (offset = i960 address)
--   tag 2  68000 wrote an SCSP register   (offset = register, data = value | mask << 16)
--          (not the slot monitor 0x408: the driver's sample streaming polls it
--          ~50,000 times a second, which would slow MAME to a crawl)
--   tag 3  68000 read an SCSP register    (offset = register, data = value | repeats << 16);
--          a read is only recorded when it differs from the last read of that
--          register (repeats = identical reads folded into it)
--   tag 4  68000 fetched an interrupt vector (offset = level)
-- plus one mark per frame: frame number, i960 frame_counter, t, record count,
-- and snapshots of the driver's work RAM and the SCSP register block.

local M = {}
_G.SNDCAP = M

M.want   = 3600
M.limit  = 40000000
M.state  = "idle"
M.n      = 0
M.marks  = {}
M.err    = nil

local main, snd, scr
local function mainsp()
    if not main then main = manager.machine.devices[":maincpu"].spaces["program"] end
    return main
end
local function sndsp()
    if not snd then snd = manager.machine.devices[":audiocpu"].spaces["program"] end
    return snd
end
local function screen()
    if not scr then scr = manager.machine.screens[":screen"] end
    return scr
end

local CLK = 11289600.0
local function now() return math.floor(manager.machine.time:as_double() * CLK + 0.5) & 0xffffffff end

function M.frame_counter() return mainsp():read_u32(0x500020) end

local inside = false
local function snapshot()
    inside = true
    local s = sndsp()
    M.ramf:write(s:read_range(0x1000, 0x4fff, 8))
    -- skip 0x404 (reading MIBUF pops the MIDI input FIFO) and 0x408 (the monitor latch)
    local a = s:read_range(0x100000, 0x100403, 16, 2)
    local b = s:read_range(0x10040a, 0x10042f, 16, 2)
    M.regsf:write(a .. string.rep("\0", 6) .. b)
    inside = false
end

function M.tick()
    if M.state ~= "capturing" then return end
    M.marks[#M.marks + 1] = { screen():frame_number(), M.frame_counter(), now(), M.n }
    snapshot()
    if #M.marks > M.want or M.n >= M.limit then M.stop() end
end

local function safetick()
    local ok, e = pcall(M.tick)
    if not ok then M.err = tostring(e); M.state = "error" end
end

function M.start(path)
    if M.taps then return "already" end
    M.path = path
    M.f = assert(io.open(path .. ".bin", "wb"))
    local buf, nb = {}, 0
    local f = M.f
    local pcst = manager.machine.devices[":audiocpu"].state["PC"]
    local pack = string.pack
    local function put(tag, off, data, pc)
        nb = nb + 1
        buf[nb] = pack("<I4I4I4I4", (tag << 24) | (off & 0xffffff), data & 0xffffffff, now(), pc)
        if nb == 4096 then f:write(table.concat(buf)); buf = {}; nb = 0 end
        M.n = M.n + 1
    end
    M.flush = function() if nb > 0 then f:write(table.concat(buf)); buf = {}; nb = 0 end; f:flush() end
    local lastr, reps = {}, {}
    M.taps = {
        mainsp():install_write_tap(0x009c0000, 0x009c0007, "midi", function(offset, data, mask)
            put(1, offset, (data & 0xff) | ((mask & 0xff) << 16), 0) end),
        sndsp():install_write_tap(0x100000, 0x100407, "scspw", function(offset, data, mask)
            if not inside then put(2, offset - 0x100000, (data & 0xffff) | ((mask & 0xffff) << 16), pcst.value) end end),
        sndsp():install_write_tap(0x10040a, 0x100fff, "scspw2", function(offset, data, mask)
            if not inside then put(2, offset - 0x100000, (data & 0xffff) | ((mask & 0xffff) << 16), pcst.value) end end),
        sndsp():install_read_tap(0x10040a, 0x100fff, "scspr2", function(offset, data, mask)
            if not inside then
                local o = offset - 0x100000
                local v = data & mask
                if lastr[o] ~= v then
                    put(3, o, (v & 0xffff) | ((math.min(reps[o] or 0, 0xffff)) << 16), pcst.value)
                    lastr[o] = v; reps[o] = 0
                else
                    reps[o] = (reps[o] or 0) + 1
                end
            end
            return data end),
        sndsp():install_read_tap(0x100000, 0x100407, "scspr", function(offset, data, mask)
            if not inside then
                local o = offset - 0x100000
                local v = data & mask
                if lastr[o] ~= v then
                    put(3, o, (v & 0xffff) | ((math.min(reps[o] or 0, 0xffff)) << 16), pcst.value)
                    lastr[o] = v; reps[o] = 0
                else
                    reps[o] = (reps[o] or 0) + 1
                end
            end
            return data end),
        sndsp():install_read_tap(0x000060, 0x00007f, "vec", function(offset, data, mask)
            if not inside and (offset & 3) == 0 then put(4, (offset - 0x60) // 4, data, pcst.value) end
            return data end),
    }
    M.ramf  = assert(io.open(path .. ".ram.bin", "wb"))
    M.regsf = assert(io.open(path .. ".regs.bin", "wb"))
    M.sub = emu.add_machine_frame_notifier(safetick)
    M.state = "capturing"
    return "ok"
end

function M.stop()
    if M.taps then for _, t in ipairs(M.taps) do t:remove() end; M.taps = nil end
    if M.state == "capturing" then
        M.flush(); M.f:close(); M.ramf:close(); M.regsf:close()
        M.state = "captured"
    end
    return "ok"
end

function M.write()
    local m = assert(io.open(M.path .. ".json", "w"))
    m:write('{"source":"mame-snd","records":', M.n, ',"ram_base":4096,"ram_size":16384,"regs_words":536,"marks":[')
    for i, mk in ipairs(M.marks) do
        if i > 1 then m:write(",") end
        m:write("[", mk[1], ",", mk[2], ",", mk[3], ",", mk[4], "]")
    end
    m:write("]}")
    m:close()
    return "ok " .. M.n .. " records, " .. #M.marks .. " marks"
end

return "SNDCAP loaded"
