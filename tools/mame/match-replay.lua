-- match_replay on MAME: take sfight's attract mode straight to its preprogrammed
-- Sonic vs Bean replay (the same jump m2hle's --match-replay makes, from
-- game_quirks_t.attract_replay in src/profiles/sfight.h), then record both
-- fighters' work structures at every frame edge, and exit.
--
-- Run it as an autoboot script, headless:
--   MR_OUT=<file> MR_FRAMES=1300 mame sfight -rompath <zips> -nodrc -video none
--     -sound none -nothrottle -skip_gameinfo -seconds_to_run 299
--     -autoboot_script tools/mame/match-replay.lua
-- tools/match-replay.mjs --mame does exactly that.
--
-- -nodrc: this MAME's SHARC recompiler fails the COP self-test (the game hangs
-- in co_processor_error_hang). -seconds_to_run under 300 is what makes MAME skip
-- its "this system doesn't work" notice with no window to press a key in.
--
-- The frame edge is variable_diff_calc's first store (dword_50D000), the point
-- m2hle's frame_pace hook (0x11A04) marks a frame at, so both sides jump and
-- sample at the same instruction of the same frame.
--
-- Output, one record per frame from the jump on:
--   u32 frame_counter, u8 stage_num, u8 attract step, u16 0,
--   MR_ROB (hex, default 0x3400) bytes of fighter 0 (0x510D00), then of fighter 1 (0x514100),
--   then each MR_EXTRA range ("hexaddr:hexlen,...") in order -- bufferram the
--   coprocessor writes and the i960 reads back directly, not through the FIFO
--
-- Optional:
--   MR_STAGE=n       play the replay on stage n: the replay's own stores of its
--                    stage (ADV_REPLAY_INT stores byte_50005B, then stage_num,
--                    then calls change_scene at 0x941C) are given n instead.
--                    m2hle's --match-replay-stage writes n to both at 0x941C,
--                    which leaves memory the same when change_scene reads it.
--   MR_SNAP=a:b:s    a snapshot of the screen at replay frames a, a+s, ... <= b
--                    (frames counted from the jump, as the records are), named
--                    r<frame>.png in -snapshot_directory (tools/grade-zsort.mjs).

local OUT    = assert(os.getenv("MR_OUT"), "set MR_OUT")
local FRAMES = tonumber(os.getenv("MR_FRAMES") or "1300")

local STEP_ADDR, FROM_STEP, TO_STEP = 0x500030, 5, 6
local READY_ADDR, STATE_ADDR = 0x5004CC, 0x5004C4
local STATE = { 0x000301A7, 0x00000028, 0x00055DDC, 0x000562D0, 0xC1200000,
                0x433A8000, 0x43810000, 0xC1200000, 0x43398000 }
local ROBS = { 0x510D00, 0x514100 }
local ROB = tonumber(os.getenv("MR_ROB") or "3400", 16)
local EXTRA = {}
for a, l in string.gmatch(os.getenv("MR_EXTRA") or "", "(%x+):(%x+)") do
    EXTRA[#EXTRA + 1] = { tonumber(a, 16), tonumber(l, 16) }
end

local STAGE = tonumber(os.getenv("MR_STAGE") or "")
local SNAP_A, SNAP_B, SNAP_S = string.match(os.getenv("MR_SNAP") or "", "^(%d+):(%d+):(%d+)$")
SNAP_A, SNAP_B, SNAP_S = tonumber(SNAP_A), tonumber(SNAP_B), tonumber(SNAP_S)

local sp = manager.machine.devices[":maincpu"].spaces["program"]
local f = assert(io.open(OUT, "wb"))
local log = assert(io.open(OUT .. ".log", "w"))
local jumped, n = false, 0

local function edge(offset, data, mask)
    if not jumped then
        if sp:read_u8(STEP_ADDR) == FROM_STEP and sp:read_u32(READY_ADDR) ~= 0 then
            for i, w in ipairs(STATE) do sp:write_u32(STATE_ADDR + 4 * (i - 1), w) end
            sp:write_u8(STEP_ADDR, TO_STEP)
            jumped = true
            log:write(string.format("jumped at frame_counter %d\n", sp:read_u32(0x500020)))
            log:flush()
        end
        return nil
    end
    if SNAP_A and n >= SNAP_A and n <= SNAP_B and (n - SNAP_A) % SNAP_S == 0 then
        manager.machine.screens[":screen"]:snapshot(string.format("r%05d.png", n))
    end
    f:write(string.pack("<I4BBI2", sp:read_u32(0x500020), sp:read_u8(0x500064), sp:read_u8(STEP_ADDR), 0))
    for _, base in ipairs(ROBS) do f:write(sp:read_range(base, base + ROB - 1, 8)) end
    for _, e in ipairs(EXTRA) do f:write(sp:read_range(e[1], e[1] + e[2] - 1, 8)) end
    n = n + 1
    if n % 120 == 0 then f:flush(); log:write("frames " .. n .. "\n"); log:flush() end
    if n >= FRAMES then
        f:close()
        log:write("done " .. n .. "\n")
        log:close()
        manager.machine:exit()
    end
    return nil
end

_G.MATCH_REPLAY_TAP = sp:install_write_tap(0x50D000, 0x50D003, "match_replay", edge)

-- The stage substitution: the first store to each byte after the jump. The
-- bus is 32 bits wide; byte_50005B is the top lane of 0x500058 and stage_num
-- the bottom lane of 0x500064.
if STAGE then
    local lanes = { [0x500058] = 24, [0x500064] = 0 }
    local pending = { [0x500058] = true, [0x500064] = true }
    local function sub(offset, data, mask)
        local sh = lanes[offset]
        if not jumped or not sh or not pending[offset] or ((mask >> sh) & 0xFF) == 0 then return nil end
        pending[offset] = nil
        log:write(string.format("stage %d substituted at 0x%X (was %d)\n", STAGE, offset + (sh // 8), (data >> sh) & 0xFF))
        log:flush()
        return (data & ~(0xFF << sh)) | ((STAGE & 0xFF) << sh)
    end
    _G.MATCH_REPLAY_STAGE_TAP = sp:install_write_tap(0x500058, 0x500067, "match_replay_stage", sub)
end
