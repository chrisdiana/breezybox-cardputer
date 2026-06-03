local breezy = require("breezy")
local spi = breezy.spi

local SCLK_PIN = -1
local MOSI_PIN = -1
local MISO_PIN = -1
local CS_PIN = -1

if SCLK_PIN < 0 or MOSI_PIN < 0 or MISO_PIN < 0 or CS_PIN < 0 then
  print("edit /root/lua/spi_demo.lua first")
  print("set SCLK_PIN MOSI_PIN MISO_PIN CS_PIN to your wired SPI pins")
  return
end

spi.open(SCLK_PIN, MOSI_PIN, MISO_PIN, CS_PIN, { mode = 0, delay_us = 1 })

print("sending one test byte 0x9F")
local rx = spi.transfer(string.char(0x9F))
if rx then
  print("got", #rx, "byte(s)")
  for i = 1, #rx do
    io.write(string.format("0x%02X ", string.byte(rx, i)))
  end
  print()
end

spi.close()
