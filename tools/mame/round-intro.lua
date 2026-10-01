-- A 1P round on a forced stage on MAME, for grade-round-intro.mjs: the round
-- intro's camera sweep, pictured on both boards.
--
--   RD_OUT=<prefix> RD_STAGE=5 RD_FRAMES=1250 RD_SNAP=745:1240:3
--   mame sfight -rompath <zips> -nodrc -video none -sound none -nothrottle
--     -skip_gameinfo -seconds_to_run 299 -snapshot_directory <dir>
--     -autoboot_script tools/mame/round-intro.lua
--
-- Inputs follow the frame count from power-on, as the grader plays them here:
-- a coin every 600 frames from 300, Start every 60, Punch and Kick between.
-- Once the game is past its title (frame 320), every byte written to
-- STAGE_NUM (0x500064) becomes RD_STAGE, so ROUND_INIT loads that stage.
-- Earlier the substitution froze the title screen.
--
-- <prefix> gets one 22-byte record per frame edge (a write to 0x50D000):
-- frame_counter (0x500020, u32), STAGE_NUM, sub-mode (0x500030), then the
-- 16 camera bytes at 0x519E98. Record n is m2hle's board frame n - 1, and
-- the camera words are bit-identical there. Frames in RD_SNAP (first:last:step)
-- are snapshotted as f<n>.png; <prefix>.log ends "done".

local OUT    = assert(os.getenv("RD_OUT"), "set RD_OUT")
local STAGE  = tonumber(os.getenv("RD_STAGE") or "5")
local FRAMES = tonumber(os.getenv("RD_FRAMES") or "1250")
local A, B, S = string.match(os.getenv("RD_SNAP") or "", "^(%d+):(%d+):(%d+)$")
A, B, S = tonumber(A), tonumber(B), tonumber(S)

local sp = manager.machine.devices[":maincpu"].spaces["program"]
local f = assert(io.open(OUT, "wb"))
local log = assert(io.open(OUT .. ".log", "w"))
local fields = {}
for _, port in pairs(manager.machine.ioport.ports) do
  for name, fld in pairs(port.fields) do fields[name] = fld end
end
local COIN, START = fields["Coin 1"], fields["1 Player Start"]
local B1, B2 = fields["P1 Punch"], fields["P1 Kick"]
log:write(string.format("coin %s start %s b1 %s b2 %s\n",
  tostring(COIN), tostring(START), tostring(B1), tostring(B2)))

local n = 0
local function set(fl, on) if fl then fl:set_value(on and 1 or 0) end end
local function edge(offset, data, mask)
  local m = n % 60
  set(COIN, n > 300 and n % 600 < 8)
  set(START, n > 300 and m < 6)
  set(B1, n > 300 and m >= 20 and m < 26)
  set(B2, n > 300 and m >= 40 and m < 46)
  if A and n >= A and n <= B and (n - A) % S == 0 then
    manager.machine.screens[":screen"]:snapshot(string.format("f%05d.png", n))
  end
  f:write(string.pack("<I4BB", sp:read_u32(0x500020), sp:read_u8(0x500064), sp:read_u8(0x500030)))
  f:write(sp:read_range(0x519E98, 0x519EA7, 8))
  n = n + 1
  if n >= FRAMES then f:close(); log:write("done\n"); log:close(); manager.machine:exit() end
  return nil
end
_G.RD_TAP = sp:install_write_tap(0x50D000, 0x50D003, "rd_edge", edge)

local function stage(offset, data, mask)
  if (mask & 0xFF) == 0 or n < 320 then return nil end
  return (data & ~0xFF) | STAGE
end
_G.RD_STAGE_TAP = sp:install_write_tap(0x500064, 0x500067, "rd_stage", stage)
