-- Capture the i960<->COP conversation off MAME in bus order, for replaying the
-- command stream through m2-hle2's COP HLE and checking every reply word:
--   record (addr, val) with addr =
--     0x20000000                  the SHARC read an argument word from its input FIFO
--     0x21000000                  ... read at the command loop (PM 0x20141): a command word
--     0x40PPOO00|..               matrix snapshot before command OO, left by command PP: value = slot ptr,
--                                 then 12 records 0x41000000 carrying the slot's words
--     0x30000000                  the SHARC wrote a word to its output FIFO
--     0x900000..0x97ffff          the i960 wrote into bufferram (SHARC DM 0x1400000)
-- plus one mark per frame edge (frame number, record count, work-RAM probes) and
-- a bufferram snapshot at the moment capture starts.

local M = {}
_G.COPCAP = M

M.from   = 0
M.want   = 1800
M.limit  = 12000000
M.state  = "idle"
M.n      = 0
M.words  = {}
M.offs   = {}
M.marks  = {}
M.probes = {}
M.snap   = nil
M.slots  = {}           -- per mark: P1+P2 TGP slots out of bufferram (i960 0x90E800 / 0x90EC00), 2*16*12 words
M.unit   = {}           -- per mark: the COP unit-matrix cache, SHARC DM 0x30420, 32*12 words (if readable)
M.err    = nil

local sp, scr
local function space()
    if not sp then sp = manager.machine.devices[":maincpu"].spaces["program"] end
    return sp
end
local function screen()
    if not scr then scr = manager.machine.screens[":screen"] end
    return scr
end

function M.set_probes(spec)
    M.probes = {}
    for a, s in string.gmatch(spec, "(%x+):(%d)") do
        M.probes[#M.probes + 1] = { tonumber(a, 16), tonumber(s) }
    end
    return #M.probes
end

local function read_probes()
    local s, out = space(), {}
    for i, p in ipairs(M.probes) do
        local a, sz = p[1], p[2]
        out[i] = sz == 1 and s:read_u8(a) or sz == 2 and s:read_u16(a) or s:read_u32(a)
    end
    return out
end

local cop
local function codata()
    if not cop then cop = manager.machine.devices[":copro_adsp"].spaces["data"] end
    return cop
end

local function read_slots()
    local s, out = space(), {}
    for _, base in ipairs({ 0x90E800, 0x90EC00 }) do
        for i = 0, 16 * 12 - 1 do out[#out + 1] = s:read_u32(base + i * 4) end
    end
    return out
end

local function read_unit()
    local c, out = codata(), {}
    for i = 0, 32 * 12 - 1 do out[#out + 1] = c:read_u32(0x30420 + i) end
    return out
end

function M.frame_counter() return space():read_u32(0x500020) end

function M.tick()
    if M.state == "armed" and M.frame_counter() >= M.from then M.start() end
    if M.state == "capturing" then
        local mk = { screen():frame_number(), M.n }
        for _, v in ipairs(read_probes()) do mk[#mk + 1] = v end
        M.marks[#M.marks + 1] = mk
        M.slots[#M.slots + 1] = read_slots()
        local ok, u = pcall(read_unit)
        M.unit[#M.unit + 1] = ok and u or {}
        if #M.marks > M.want or M.n >= M.limit then M.stop() end
    end
end

local function safetick()
    local ok, e = pcall(M.tick)
    if not ok then M.err = tostring(e); M.state = "error" end
end

function M.attach()
    if M.sub then return "already" end
    M.sub = emu.add_machine_frame_notifier(safetick)
    M.state = "armed"
    return "ok"
end

function M.start()
    if M.taps then return "already" end
    M.n, M.words, M.offs, M.marks, M.slots, M.unit = 0, {}, {}, {}, {}, {}
    local s = space()
    local snap = {}
    for a = 0x900000, 0x91fffc, 4 do snap[#snap + 1] = s:read_u32(a) end
    M.snap = snap
    local words, offs = M.words, M.offs
    local function rec(addr, data)
        local n = M.n + 1
        M.n = n
        words[n] = data
        offs[n] = addr
    end
    -- The SHARC's side of the two FIFOs: what the firmware actually consumed and
    -- produced, in order. (The i960 side is useless for replies: an empty FIFO
    -- stalls the i960 and the tap sees a 0 read before the retry.)
    local c = codata()
    local pcst = manager.machine.devices[":copro_adsp"].state["PC"]
    local prev = 0
    local SNAP = { [0x03]=true, [0x06]=true, [0x07]=true, [0x08]=true, [0x09]=true, [0x0a]=true,
                   [0x0b]=true, [0x0e]=true, [0x35]=true, [0x36]=true, [0x37]=true, [0x3f]=true,
                   [0x44]=true, [0x45]=true, [0x46]=true, [0x62]=true, [0x67]=true, [0x69]=true,
                   [0x6b]=true, [0x73]=true, [0x74]=true, [0x7a]=true, [0x7e]=true, [0x04]=true, [0x11]=true }
    M.taps = {
        -- PM 0x20141 is the command loop's read: tag command words 0x21000000.
        -- At a command word, the previous command has finished: when that one
        -- was a draw or a rig op (SNAP), or this one is a draw, record the
        -- current matrix the firmware holds (DM[DM[0x3033F]], 12 words) as
        -- 0x40000000|op (stack pointer) followed by 12 x 0x41000000.
        c:install_read_tap(0x0400000, 0x0bfffff, "shin", function(offset, data, mask)
            if pcst.value == 0x20141 then
                local op = data & 0xff
                if SNAP[prev] or op == 0x78 or op == 0x37 or op == 0x74 then
                    local i7 = c:read_u32(0x3033f)
                    rec(0x40000000 | (prev << 8) | op, i7)
                    for k = 0, 11 do rec(0x41000000, c:read_u32(i7 + k)) end
                end
                prev = op
                rec(0x21000000, data)
            else
                rec(0x20000000, data)
            end
            return data end),
        c:install_write_tap(0x0c00000, 0x13fffff, "shout", function(offset, data, mask) rec(0x30000000, data) end),
        s:install_write_tap(0x00900000, 0x0097ffff, "bufw", function(offset, data, mask) rec(offset, data) end),
    }
    M.state = "capturing"
    return "ok"
end

function M.stop()
    if M.taps then for _, t in ipairs(M.taps) do t:remove() end; M.taps = nil end
    if M.state == "capturing" then M.state = "captured" end
    return "ok"
end

function M.write(path)
    local f = assert(io.open(path .. ".bin", "wb"))
    local chunk = {}
    for i = 1, M.n do
        chunk[#chunk + 1] = string.pack("<I4I4", M.offs[i], M.words[i])
        if #chunk == 8192 then f:write(table.concat(chunk)); chunk = {} end
    end
    if #chunk > 0 then f:write(table.concat(chunk)) end
    f:close()

    local b = assert(io.open(path .. ".bufram.bin", "wb"))
    local c = {}
    for i, w in ipairs(M.snap or {}) do c[i] = string.pack("<I4", w) end
    b:write(table.concat(c))
    b:close()

    for _, pair in ipairs({ { ".tgp.bin", M.slots }, { ".unit.bin", M.unit } }) do
        local t = assert(io.open(path .. pair[1], "wb"))
        for _, frame in ipairs(pair[2]) do
            local c = {}
            for i = 1, 768 do c[i] = string.pack("<I4", frame[i] or 0) end
            t:write(table.concat(c))
        end
        t:close()
    end

    local m = assert(io.open(path .. ".json", "w"))
    m:write('{"source":"mame-cop","records":', M.n, ',"frames":', math.max(0, #M.marks - 1), ',"marks":[')
    for i, mk in ipairs(M.marks) do
        if i > 1 then m:write(",") end
        m:write("[")
        for j, v in ipairs(mk) do
            if j > 1 then m:write(",") end
            m:write(tostring(v))
        end
        m:write("]")
    end
    m:write("]}")
    m:close()
    return "ok " .. M.n .. " records, " .. #M.marks .. " marks"
end

return "COPCAP loaded"
