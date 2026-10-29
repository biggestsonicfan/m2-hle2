-- mame-sharc-tap-fight.lua: tools/mame-sharc-tap.lua's capture, with a scripted
-- 1P fight on Fighting Vipers (or any game on MAME's vf2 inputs), so a capture
-- of play needs nobody at the controls (Pinboard #1349). Same 9-byte records.
-- Coin 1 from frame 600, start from 700; from 1400 a 40-frame loop of
-- guard+punch, down, down+kick, right+punch. On fvipers (Grace against Bahn,
-- K.O. Corral) the CPU launches P1 at frame ~1830 and she lands by ~1900; more
-- launches follow, a K.O. at ~3360, round 2 from ~3500.
--
--   SHARC_TAP_OUT=/dev/shm/x/cop.bin SHARC_TAP_ROM=/dev/shm/x/copro_data.bin \
--   TAP_FRAMES=7000 SHOT_EVERY=20 mame fvipers -nodrc -video none -sound none \
--     -nothrottle -snapshot_directory /dev/shm/x/snap \
--     -autoboot_script tools/mame-sharc-tap-fight.lua
--
-- SHOT_EVERY=n saves the screen every n frames (to see the launches).
local N = tonumber(os.getenv("TAP_FRAMES") or "7000")
local SHOT = tonumber(os.getenv("SHOT_EVERY") or "0")
local n = 0
local m = manager.machine
local f = assert(io.open(assert(os.getenv("SHARC_TAP_OUT"), "set SHARC_TAP_OUT"), "wb"))
local sp = m.devices[":maincpu"].spaces["program"]
local dp = m.devices[":copro_adsp"].spaces["data"]
local rom_out = os.getenv("SHARC_TAP_ROM")
if rom_out then
  local rf = assert(io.open(rom_out, "wb"))
  local reg = m.memory.regions[":copro_data"]
  for a = 0, reg.size - 4, 4 do rf:write(string.pack("<I4", reg:read_u32(a))) end
  rf:close()
end
local function rec(t, a, v) f:write(string.pack("<BI4I4", t, a, v)) end
_G.T1 = sp:install_write_tap(0x880000, 0x887fff, "cw", function(off, data, mask) rec(1, off, data) end)
_G.T2 = sp:install_write_tap(0x980000, 0x980003, "ctl", function(off, data, mask) rec(2, off, data) end)
_G.T3 = sp:install_write_tap(0x900000, 0x91ffff, "buf", function(off, data, mask) rec(3, off, data) end)
_G.T4 = dp:install_write_tap(0x0c00000, 0x13fffff, "out", function(off, data, mask) rec(4, off, data) end)
local ports = m.ioport.ports
local F = {
  coin = ports[":IN0"].fields["Coin 1"], start = ports[":IN0"].fields["1 Player Start"],
  P = ports[":IN1"].fields["P1 Punch"], K = ports[":IN1"].fields["P1 Kick"], G = ports[":IN1"].fields["P1 Guard"],
  D = ports[":IN1"].fields["P1 Down"], R = ports[":IN1"].fields["P1 Right"],
}
for k, v in pairs(F) do assert(v, k) end
-- The fields held at frame n: the coin, the start and the fight loop above.
local function fight_inputs(n)
  local want = {}
  if n > 600 and n % 900 < 6 then want.coin = 1 end
  if n > 700 and n % 120 < 4 then want.start = 1 end
  if n > 1400 then
    local q = n % 40
    if q < 6 then want.G = 1; want.P = 1
    elseif q >= 10 and q < 14 then want.D = 1
    elseif q >= 14 and q < 18 then want.D = 1; want.K = 1
    elseif q >= 24 and q < 28 then want.R = 1; want.P = 1 end
  end
  return want
end
_G.F = emu.add_machine_frame_notifier(function()
  n = n + 1
  rec(0, n, 0)
  local want = fight_inputs(n)
  for k, fld in pairs(F) do fld:set_value(want[k] or 0) end
  if SHOT > 0 and n % SHOT == 0 then m.screens[":screen"]:snapshot(string.format("f%05d.png", n)) end
  if n >= N then f:close(); m:exit() end
end)
