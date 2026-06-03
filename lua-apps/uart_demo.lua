local breezy = require("breezy")
local uart = breezy.uart

local PORT = 1
local BAUD = 115200
local TX_PIN = -1
local RX_PIN = -1

if TX_PIN < 0 or RX_PIN < 0 then
  print("edit /root/lua/uart_demo.lua first")
  print("set PORT to 1 or 2 and choose free TX_PIN / RX_PIN values")
  return
end

local handle = uart.open(PORT, BAUD, TX_PIN, RX_PIN)
print("uart opened:", handle, "baud:", BAUD)
print("type in another device and press q here to quit")

while true do
  local data = uart.read(handle, 64, 20)
  if data then
    print("rx:", data)
  end

  local key = breezy.readkey(0)
  if key == "q" then
    break
  elseif key and #key > 0 then
    uart.write(handle, key)
  end

  breezy.sleep_ms(10)
end

uart.close(handle)
print("uart closed")
