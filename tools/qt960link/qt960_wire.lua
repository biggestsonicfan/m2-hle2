-- qt960_wire.lua — MAME autoboot script for the qt960 driver: the QT960's serial cable
-- as a TCP port, so qtlink_host.py can drive the emulated board exactly as it drives the
-- real one on a COM port (Pinboard #316).
--
--   QTWIRE_PORT=7961 mame qt960 -autoboot_script qt960_wire.lua
--   python3 qtlink_host.py --board tcp:127.0.0.1:7961 --m2k tcp:127.0.0.1:7960
--
-- Bytes the board writes to the 82510 go out on the socket; bytes from the socket are
-- what it reads, one at a time, with LSR's data-ready bit up while any wait. Nothing else:
-- no loading, no relay. The driver's terminal still shows everything the board prints.
-- One client at a time; when it hangs up, the port listens again.
if QTWIRE_ON then return end   -- NINDY's own reset after its self-test runs this again
QTWIRE_ON = true

local getenv = os.getenv or function() return nil end
local port = getenv("QTWIRE_PORT") or "7961"
local DATA, LSR = 0x20000000, 0x20000014

local space = manager.machine.devices[":maincpu"].spaces["program"]
local function log(s) print("qt960_wire: " .. s) end

local rxq, rxh = {}, 1        -- bytes for the board, and the next one to read
local txq = {}                -- bytes from the board, not yet on the socket
local sock

local function listen()
  local f = emu.file("rwc")
  local err = f:open("socket.127.0.0.1:" .. port)
  if err then log("can't listen on port " .. port .. " (" .. tostring(err) .. ")"); return nil end
  return f
end

local function poll()
  if not sock then return end
  local got = sock:read(4096)
  if #got > 0 then
    if rxh > #rxq then rxq, rxh = {}, 1 end
    for i = 1, #got do rxq[#rxq + 1] = got:byte(i) end
  end
end

local polls = 0
QTWIRE_RD = space:install_read_tap(DATA, DATA + 0x1F, "qtwire_rx", function(offset, data, mask)
  if offset == LSR then
    if rxh > #rxq then
      polls = polls + 1
      if polls >= 256 then polls = 0; poll() end
    end
    if rxh <= #rxq then return data | 1 end
  elseif offset == DATA and rxh <= #rxq then
    local b = rxq[rxh]; rxh = rxh + 1
    return b
  end
end)
QTWIRE_WR = space:install_write_tap(DATA, DATA + 3, "qtwire_tx", function(offset, data, mask)
  txq[#txq + 1] = string.char(data & 0xFF)
end)

QTWIRE_TICK = emu.register_periodic(function()
  if not sock then return end
  if #txq > 0 then sock:write(table.concat(txq)); txq = {} end
  poll()
end)

sock = listen()
if sock then log("the QT960's serial port is on 127.0.0.1:" .. port) end
