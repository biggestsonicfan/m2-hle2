-- mame-sharc-tap.lua: capture a Model 2B game's SHARC COP traffic in MAME
-- for tests/sharc_replay.c (Pinboard #949). Records are 9 bytes, <BI4I4:
-- kind, address, value. 0 frame end, 1 i960 write 0x880000-0x887FFF (function
-- port / FIFO in / boot upload), 2 COPRO_CTL, 3 buffer RAM, 4 SHARC FIFO out.
-- The SHARC's read side is not tapped: MAME retries a pop of an empty FIFO,
-- so a read tap fires twice; pushes are exact.
--
--   SHARC_TAP_OUT=/dev/shm/x/cop.bin SHARC_TAP_ROM=/dev/shm/x/copro_data.bin \
--   TAP_FRAMES=4200 mame vonj -nodrc -video none -sound none -nothrottle \
--     -autoboot_script tools/mame-sharc-tap.lua
--
-- -nodrc: the interpreter is the oracle sharc_cpu.h follows.
local N = tonumber(os.getenv("TAP_FRAMES") or "4200")
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
_G.F = emu.add_machine_frame_notifier(function()
  n = n + 1
  rec(0, n, 0)
  if n >= N then f:close(); m:exit() end
end)
