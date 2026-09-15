-- Character select on MAME, for the sway chains: coin up, walk P1's cursor onto
-- each fighter in OS_CHARS, and for each one take a SHARC-side capture of the
-- coprocessor conversation (cop-capture.lua) plus a screen snapshot every
-- OS_EVERY frames while the model turns. The select screen draws its fighter
-- through Fn_osage like a fight does, but from a pose and camera attract never
-- reaches, so it is the osage scene the attract captures do not cover.
--
-- Run it as an autoboot script, headless:
--   OS_OUT=<prefix> OS_CHARS=4,10 mame sfight -rompath <zips> -nodrc -video none
--     -sound none -nothrottle -skip_gameinfo -seconds_to_run 290
--     -snapshot_directory <dir> -autoboot_script tools/mame/osage-select.lua
--
-- Output per fighter c: <prefix>-c<c>.bin/.bufram.bin/.dm.bin/... (cop_replay's
-- input) and snapshots sfight/NNNN.png, listed in <prefix>.log with their frame.

local OUT    = assert(os.getenv("OS_OUT"), "set OS_OUT")
local CHARS  = {}
for c in string.gmatch(os.getenv("OS_CHARS") or "4,10", "%d+") do CHARS[#CHARS + 1] = tonumber(c) end
local FRAMES = tonumber(os.getenv("OS_FRAMES") or "120")
local EVERY  = tonumber(os.getenv("OS_EVERY") or "15")
local COIN_AT = tonumber(os.getenv("OS_COIN_AT") or "1800")
local HERE = debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])") or "./"

dofile(HERE .. "cop-capture.lua")
local CAP = _G.COPCAP

local sp  = manager.machine.devices[":maincpu"].spaces["program"]
local in0 = manager.machine.ioport.ports[":IN0"]
local in1 = manager.machine.ioport.ports[":IN1"]
local log = assert(io.open(OUT .. ".log", "w"))
local function say(...) log:write(string.format(...), "\n"); log:flush() end

local ROB_CHAR, MODE, SUB = 0x510D00 + 0x1B0, 0x50002A, 0x500030

-- a list of { frames to wait, action } run one after another off the frame notifier
local frame, wait, script, pos = 0, 0, {}, 1
local held = nil
local function press(port, field, frames)
    script[#script + 1] = function() port.fields[field]:set_value(1); held = { port, field }; return frames end
    script[#script + 1] = function() held[1].fields[held[2]]:set_value(0); held = nil; return frames end
end
local function after(frames, fn) script[#script + 1] = function() if fn then local r = fn(); if r then return r end end; return frames end end

after(COIN_AT)
press(in0, "Coin 1", 8)
after(60)
press(in0, "1 Player Start", 8)
-- the real select screen, not attract's demo of it: mode 7, sub 5, some frames after Start
after(1, function()
    if not (sp:read_u8(MODE) == 7 and sp:read_u8(SUB) == 5) then pos = pos - 1; return 1 end
    say("select screen at frame %d (frame_counter %d)", frame, sp:read_u32(0x500020))
    return 400
end)

for _, c in ipairs(CHARS) do
    local tries, down = 0, false
    after(1, function()
        -- tap right (10 frames down, 10 up) until the cursor is on c
        if down then in1.fields["P1 Right"]:set_value(0); down = false; pos = pos - 1; return 10 end
        if sp:read_u8(ROB_CHAR) ~= c and tries < 12 then
            tries = tries + 1
            in1.fields["P1 Right"]:set_value(1); down = true; pos = pos - 1; return 10
        end
        return 1
    end)
    after(40, function()
        say("char %d: rob char %d at frame %d", c, sp:read_u8(ROB_CHAR), frame)
        CAP.start()
        return 1
    end)
    for k = 0, FRAMES - 1 do
        after(1, function()
            if k % EVERY == 0 then
                manager.machine.video:snapshot()
                say("  snapshot c%d k%d frame %d frame_counter %d", c, k, frame, sp:read_u32(0x500020))
            end
            return 1
        end)
    end
    after(1, function()
        CAP.stop()
        say("char %d: %s", c, CAP.write(string.format("%s-c%d", OUT, c)))
        return 1
    end)
end
after(1, function() say("done"); log:close(); manager.machine:exit(); return 1 end)

local function tick()
    frame = frame + 1
    if wait > 0 then wait = wait - 1; return end
    local fn = script[pos]
    if not fn then return end
    pos = pos + 1
    wait = (fn() or 1) - 1
end

_G.OSAGE_SELECT_SUB = emu.add_machine_frame_notifier(function()
    local ok, e = pcall(tick)
    if not ok then say("error: %s", tostring(e)); log:close(); manager.machine:exit() end
end)
say("osage-select armed: chars %s, %d frames each", table.concat(CHARS, ","), FRAMES)
