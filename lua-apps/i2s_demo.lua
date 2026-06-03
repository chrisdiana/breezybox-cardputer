local breezy = require("breezy")
local i2s = breezy.i2s

print("I2S demo")
print("")
print("This is a template for external I2S hardware.")
print("Edit the pins and sample format for your device.")
print("")
print("Example TX wiring:")
print("  bclk -> external device BCLK")
print("  ws   -> external device LRCLK/WS")
print("  dout -> external device DIN")
print("")

local SAMPLE_RATE = 16000
local BITS = 16
local CHANNELS = 1

-- Change these for your wiring before using the demo.
local BCLK_PIN = -1
local WS_PIN = -1
local DOUT_PIN = -1

if BCLK_PIN < 0 or WS_PIN < 0 or DOUT_PIN < 0 then
  print("Set BCLK_PIN / WS_PIN / DOUT_PIN in /root/lua/i2s_demo.lua first.")
  return
end

local function le16(v)
  if v < 0 then
    v = v + 65536
  end
  return string.char(v & 0xFF, (v >> 8) & 0xFF)
end

-- Make a tiny square wave test tone.
local function make_square(samples, amp)
  local out = {}
  local half = samples // 2
  for i = 1, samples do
    out[#out + 1] = le16(i <= half and amp or -amp)
  end
  return table.concat(out)
end

local period = SAMPLE_RATE // 440
local burst = make_square(period, 12000)

i2s.open_tx(BCLK_PIN, WS_PIN, DOUT_PIN, {
  sample_rate = SAMPLE_RATE,
  bits = BITS,
  channels = CHANNELS,
  format = "msb",
})

print("TX open. Writing short tone burst...")
for _ = 1, 120 do
  i2s.write(burst, 1000)
end

i2s.close("tx")
print("done")
