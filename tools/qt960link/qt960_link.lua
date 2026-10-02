-- qt960_link.lua — MAME autoboot script for the qt960 driver: the PC on the end of the
-- QT960's serial cable (Pinboard #312).
--
--   QTLINK_BIN=qtlink.bin mame qt960 -autoboot_script qt960_link.lua
--
-- It does what a person at a terminal would, then what a relay script on that PC would:
--   1. waits for NINDY's "=>" prompt after the self-test;
--   2. types qtlink.bin into the board with NINDY's "mo" command, a word a line, each
--      line only once NINDY has prompted for it (NINDY drops what arrives while it is
--      busy), then "go 10100000";
--   3. relays: qtlink writes each command frame for the Model 2B as a text line
--      ">A5....\r"; this script sends the bytes to m2-kernel's MAME bridge
--      (m2k_serial.lua, TCP 127.0.0.1:7960) and types the kernel's reply back as
--      "<5A....\r".
-- The terminal shows all of it. The 82510's line is reached through taps on its
-- registers (RXD/TXD at 0x20000000, LSR at 0x20000014), so the driver is unchanged.
--
-- Environment:
--   QTLINK_BIN      the image (tools/qt960link/qtlink/build.sh); default qtlink.bin
--   QTLINK_M2K      the Model 2B bridge, host:port (127.0.0.1:7960)
--   QTLINK_LOAD     "type" (default: through NINDY, as on the board) or "poke" (write
--                   SRAM directly, then type only "go": for quick tests)
--   QTLINK_ECHO=1   print qtlink's result lines on stdout too
--   QTLINK_EXIT=N   exit MAME after N checks (headless tests)
if QTLINK_ON then return end   -- NINDY's own reset after its self-test runs this again
QTLINK_ON = true

local getenv = os.getenv or function() return nil end
local BASE = 0x10100000
local DATA, LSR = 0x20000000, 0x20000014
local bin_path = getenv("QTLINK_BIN") or "qtlink.bin"
local m2k_addr = getenv("QTLINK_M2K") or "127.0.0.1:7960"
local load_mode = getenv("QTLINK_LOAD") or "type"
local echo = getenv("QTLINK_ECHO") == "1"
local exit_after = tonumber(getenv("QTLINK_EXIT") or "")

local space = manager.machine.devices[":maincpu"].spaces["program"]

local function log(s) print("qt960_link: " .. s) end

-- the image, as little-endian words
local words = {}
do
  local f = io.open(bin_path, "rb")
  if not f then log("cannot open " .. bin_path .. " (QTLINK_BIN)"); return end
  local img = f:read("a"); f:close()
  img = img .. string.rep("\0", (4 - #img % 4) % 4)
  for i = 1, #img, 4 do words[#words + 1] = string.unpack("<I4", img, i) end
  log(string.format("%s: %d words", bin_path, #words))
end

-- ---- the serial line -------------------------------------------------------------
local rxq, rxh = {}, 1        -- bytes for the board, and the next one to read
local tail = ""               -- the last few characters the board sent
local line = {}
local state = "boot"          -- boot, load, start, relay
local saw_version = false     -- NINDY's banner (its "=>" before that is the self-test's)
local next_word = 1
local sock, sock_rx, sock_retry = nil, "", 0
local pending = {}            -- frames waiting for the socket
local checks, diffs = 0, 0

local function rx_empty() return rxh > #rxq end
local function type_line(s)
  if rx_empty() then rxq, rxh = {}, 1 end
  for i = 1, #s do rxq[#rxq + 1] = s:byte(i) end
  rxq[#rxq + 1] = 13
end

local function connect()
  local f = emu.file("rw")
  if f:open("socket." .. m2k_addr) then return nil end
  log("connected to the Model 2B bridge at " .. m2k_addr)
  return f
end

local function send_frame(bytes)
  if not sock then pending[#pending + 1] = bytes; return end
  sock:write(bytes)
end

-- take whole reply frames (5A status len payload sum) off the socket, type them back
local function poll_socket()
  if not sock then return end
  local got = sock:read(4096)
  if #got == 0 then return end
  sock_rx = sock_rx .. got
  while #sock_rx >= 4 do
    if sock_rx:byte(1) ~= 0x5A then
      sock_rx = sock_rx:sub(2)
    else
      local n = 4 + sock_rx:byte(3)
      if #sock_rx < n then break end
      local fr = sock_rx:sub(1, n)
      sock_rx = sock_rx:sub(n + 1)
      type_line("<" .. fr:gsub(".", function(c) return string.format("%02X", c:byte()) end))
    end
  end
end

local function on_line(s)
  if echo and (s:find("  OK$") or s:find("  DIFF$")) then print(s) end
  if s:find("^%s*%d+ %S+ .*  OK$") then checks = checks + 1 end
  if s:find("  DIFF$") then checks = checks + 1; diffs = diffs + 1 end
  if s:find("^qtlink: .*no answer") then log(s) end
  if s:find("^READBACK") or s:find("^PING") or s:find("^STATUS") then log(s) end
  if exit_after and checks >= exit_after then
    log(string.format("%d checks, %d differences", checks, diffs))
    manager.machine:exit()
  end
end

local function on_tx(c)
  local ch = string.char(c)
  tail = (tail .. ch):sub(-32)

  if state == "relay" then
    if c == 13 or c == 10 then
      local s = table.concat(line)
      line = {}
      -- a frame is the line's tail: qtlink may send one after text it has not ended
      local hex = s:match(">(A5%x+)$")
      if hex and #hex >= 8 and #hex % 2 == 0 then
        send_frame((hex:gsub("%x%x", function(h) return string.char(tonumber(h, 16)) end)))
      elseif #s > 0 then
        on_line(s)
      end
    else
      line[#line + 1] = ch
    end
    return
  end

  -- loading: answer each prompt NINDY gives
  if not rx_empty() then return end
  if state == "boot" then
    if tail:find("Version 3") then saw_version = true end
    if saw_version and tail:find("=>$") then
      if load_mode == "poke" then
        for i, w in ipairs(words) do space:write_u32(BASE + 4 * (i - 1), w) end
        state = "start"
        log("image poked into SRAM")
      else
        state = "load"
        type_line(string.format("mo %08x %d", BASE, #words))   -- the count is decimal
      end
    end
  elseif state == "load" then
    -- "addr : old : ": wait for the second colon. NINDY reads the line while it prints
    -- (its ^S/^C check), so a word typed after the first one is eaten.
    if tail:find(" : %x+ : $") and next_word <= #words then
      type_line(string.format("%08x", words[next_word]))
      next_word = next_word + 1
    elseif tail:find("=>$") and next_word > #words then
      state = "start"
    end
  end
  if state == "start" and tail:find("=>$") then
    log(string.format("loaded %d words, go %08x", #words, BASE))
    state = "relay"
    type_line(string.format("go %08x", BASE))
  end
end

-- ---- taps on the 82510 ------------------------------------------------------------
local polls = 0
QTLINK_RD = space:install_read_tap(DATA, DATA + 0x1F, "qtlink_rx", function(offset, data, mask)
  if offset == LSR then
    if rx_empty() and sock then
      polls = polls + 1
      if polls >= 256 then polls = 0; poll_socket() end
    end
    if not rx_empty() then return data | 1 end
  elseif offset == DATA and not rx_empty() then
    local b = rxq[rxh]; rxh = rxh + 1
    return b
  end
end)
QTLINK_WR = space:install_write_tap(DATA, DATA + 3, "qtlink_tx", function(offset, data, mask)
  on_tx(data & 0xFF)
end)

-- the socket: connect (and reconnect) once a frame, flush what waited, read replies
QTLINK_TICK = emu.register_periodic(function()
  if state ~= "relay" then return end
  if not sock then
    sock_retry = sock_retry - 1
    if sock_retry > 0 then return end
    sock_retry = 60
    sock = connect()
    if not sock then return end
    for _, b in ipairs(pending) do sock:write(b) end
    pending = {}
  end
  poll_socket()
end)

log("waiting for NINDY's prompt")
