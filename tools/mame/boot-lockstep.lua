-- boot-lockstep on MAME: sfight from power-on, the board's memory at every game
-- frame edge, for tools/dc-lockstep.py --boot (Pinboard #478).
--
-- match-replay.lua starts at attract's replay fight; this starts at the first
-- frame. At every frame edge (variable_diff_calc's first store, dword_50D000:
-- the instruction m2hle's frame_pace hook marks a frame at) it writes one
-- record to BL_OUT:
--
--   "M2BF", u32 frame_counter (0x500020), u8 mode (0x50002A), u8 sub-mode
--   (0x500030), u16 0, u32 the PC of the store, then each range of BL_RANGES
--   ("hexaddr:hexlen,...") as read_range gives it.
--
-- BL_OUT is meant to be a named pipe: MAME blocks on a full pipe, so it runs
-- no further ahead of the reader than the pipe holds. BL_FRAMES (default 6000)
-- records, then MAME exits.
--
--   BL_OUT=<fifo> BL_RANGES=500000:100000,... mame sfight -rompath <zips>
--     -nodrc -video none -sound none -nothrottle -skip_gameinfo
--     -seconds_to_run 299 -autoboot_script tools/mame/boot-lockstep.lua
--
-- -seconds_to_run is what skips MAME's "this system doesn't work" notice with no
-- window; it stops a run at 299 s of emulated time (17,940 frames).
--
-- BL_SHOTS=N also saves the screen every N frames as m<frame>.png in the
-- snapshot directory (Pinboard #486), the frame numbered as dc-lockstep.py
-- numbers it: MAME's edges before the program's first (the boot code's clear
-- of 0x50D000) and the one after it are not counted.

local OUT    = assert(os.getenv("BL_OUT"), "set BL_OUT")
local FRAMES = tonumber(os.getenv("BL_FRAMES") or "6000")
local RANGES = {}
for a, l in string.gmatch(os.getenv("BL_RANGES") or "500000:100000", "(%x+):(%x+)") do
    RANGES[#RANGES + 1] = { tonumber(a, 16), tonumber(l, 16) }
end

local cpu = manager.machine.devices[":maincpu"]
local sp = cpu.spaces["program"]
local pc = cpu.state["PC"] or cpu.state["CURPC"] or cpu.state["IP"]
local SHOTS  = tonumber(os.getenv("BL_SHOTS") or "0")
local f = assert(io.open(OUT, "wb"))
local n = 0
local g                                 -- the frame as dc-lockstep.py counts it

local function shoot(pcv)
    if SHOTS <= 0 then return end
    if g then
        g = g + 1
    elseif pcv >= 0x10000 then
        g = -1
    end
    if g and g >= 0 and g % SHOTS == 0 then
        manager.machine.screens[":screen"]:snapshot(string.format("m%05d.png", g))
    end
end

local function record()
    shoot(pc and pc.value or 0)
    f:write("M2BF", string.pack("<I4BBI2I4", sp:read_u32(0x500020), sp:read_u8(0x50002A), sp:read_u8(0x500030), 0,
                                pc and pc.value or 0))
    for _, r in ipairs(RANGES) do f:write(sp:read_range(r[1], r[1] + r[2] - 1, 8)) end
    f:flush()
    n = n + 1
    if n >= FRAMES then
        f:close()
        manager.machine:exit()
    end
end

-- An error inside a tap is swallowed; say it once and carry on.
local said
local function edge(offset, data, mask)
    local ok, err = pcall(record)
    if not ok and not said then io.stderr:write("boot-lockstep: " .. tostring(err) .. "\n"); said = true end
    return nil
end

_G.BOOT_LOCKSTEP_TAP = sp:install_write_tap(0x50D000, 0x50D003, "boot_lockstep", edge)
