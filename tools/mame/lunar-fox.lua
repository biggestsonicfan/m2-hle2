-- The Death Egg II cutscene on MAME (MEZASE_DEATHEGG, the Lunar Fox taking off
-- from Tails' lab), reached the way a player reaches it: a 1P game, the fighter
-- in LF_CHAR picked at select, the story set to its ninth fight (STAGE_ID 8,
-- which STORY_STAGE_ORDER maps to stage 7, Giant Wing), and every round won
-- by P1 on time (see watch()). VIC_DSP then
-- moves to sub-mode 0x1E itself (0xEAE4). grade-lunar-fox.mjs plays the same
-- cheat on m2hle, so the same fighter flies the Lunar Fox on both boards.
--
--   LF_OUT=<prefix> LF_CHAR=0 mame sfight -rompath <zips> -nodrc -video none
--     -sound none -nothrottle -skip_gameinfo -seconds_to_run 290
--     -snapshot_directory <dir> -autoboot_script tools/mame/lunar-fox.lua
--
-- A snapshot every LF_EVERY counts of am_cntr through MEZASE_DEATHEGG_DSP,
-- listed in <prefix>.log with am_cntr.

local OUT     = assert(os.getenv("LF_OUT"), "set LF_OUT")
local CHAR    = tonumber(os.getenv("LF_CHAR") or "0")
local EVERY   = tonumber(os.getenv("LF_EVERY") or "10")
local COIN_AT = tonumber(os.getenv("LF_COIN_AT") or "1800")

local sp  = manager.machine.devices[":maincpu"].spaces["program"]
local in0 = manager.machine.ioport.ports[":IN0"]
local in1 = manager.machine.ioport.ports[":IN1"]
local log = assert(io.open(OUT .. ".log", "w"))
local function say(...) log:write(string.format(...), "\n"); log:flush() end

local MODE, SUB, STAGE_ID, FRAME = 0x50002A, 0x500030, 0x500054, 0x500020
local FA_ROB0, FA_ROB1, ENERGY, ROB_CHAR = 0x500804, 0x500808, 0x1AC, 0x510D00 + 0x1B0
local AM_CNTR, GAME_TIMER = 0x5004C4, 0x500028

local frame, wait, script, pos = 0, 0, {}, 1
local held = nil
local function press(port, field, frames)
    script[#script + 1] = function() port.fields[field]:set_value(1); held = { port, field }; return frames end
    script[#script + 1] = function() held[1].fields[held[2]]:set_value(0); held = nil; return frames end
end
local function after(frames, fn) script[#script + 1] = function() if fn then local r = fn(); if r then return r end end; return frames end end
local function until_(cond) after(1, function() if not cond() then pos = pos - 1 end; return 1 end) end

after(COIN_AT)
press(in0, "Coin 1", 8)
after(60)
press(in0, "1 Player Start", 8)
until_(function() return sp:read_u8(MODE) == 7 and sp:read_u8(SUB) == 5 end)
after(1, function() say("select at frame %d (frame_counter %d)", frame, sp:read_u32(FRAME)); return 400 end)
local tries, down = 0, false
after(1, function()
    if down then in1.fields["P1 Right"]:set_value(0); down = false; pos = pos - 1; return 10 end
    if sp:read_u8(ROB_CHAR) ~= CHAR and tries < 12 then
        tries = tries + 1; in1.fields["P1 Right"]:set_value(1); down = true; pos = pos - 1; return 10
    end
    say("char %d at frame %d", sp:read_u8(ROB_CHAR), frame)
    return 10
end)
press(in1, "P1 Kick", 8)
-- the rest runs off the per-frame hook below until the cutscene ends
after(1, function() say("picked at frame %d", frame); return 1 end)

local last_sub, last_am, done, full = -1, -1, false, nil
local function watch()
    local sub = sp:read_u8(SUB)
    if sp:read_u8(MODE) == 7 and sub <= 7 then sp:write_u8(STAGE_ID, 8) end
    -- P1 keeps full energy, P2 is left 1, and the round clock is cut to a
    -- second: the round goes to P1 on time, through the game's own judging
    -- (an energy of 0 alone is not a KO; only a hit sets the down flag).
    if sub == 8 then full = sp:read_u16(sp:read_u32(FA_ROB0) + ENERGY) end
    if sub == 9 and full then
        sp:write_u16(sp:read_u32(FA_ROB0) + ENERGY, full)
        sp:write_u16(sp:read_u32(FA_ROB1) + ENERGY, 1)
        if sp:read_i16(GAME_TIMER) > 60 then sp:write_u16(GAME_TIMER, 60) end
    end
    if sub ~= last_sub then
        say("sub %d at frame %d (frame_counter %d, mode %d, stage_num %d, am_cntr %d)", sub, frame,
            sp:read_u32(FRAME), sp:read_u8(MODE), sp:read_u8(0x500064), sp:read_u16(AM_CNTR))
        if last_sub == 33 then done = true end
        last_sub = sub
    end
    if sub == 33 then
        local am = sp:read_u16(AM_CNTR)
        if am ~= last_am and am % EVERY == 0 then
            manager.machine.video:snapshot()
            say("  snapshot am %d frame %d frame_counter %d", am, frame, sp:read_u32(FRAME))
        end
        last_am = am
    end
    if done then say("done"); log:close(); manager.machine:exit() end
end

local function tick()
    frame = frame + 1
    if pos > 3 then watch() end
    if wait > 0 then wait = wait - 1; return end
    local fn = script[pos]
    if not fn then return end
    pos = pos + 1
    wait = (fn() or 1) - 1
end

_G.LUNAR_FOX_SUB = emu.add_machine_frame_notifier(function()
    local ok, e = pcall(tick)
    if not ok then say("error: %s", tostring(e)); log:close(); manager.machine:exit() end
end)
say("lunar-fox armed: char %d, every %d", CHAR, EVERY)
