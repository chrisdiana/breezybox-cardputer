local breezy = require("breezy")
local i2c = breezy.i2c

local SDA_PIN = -1
local SCL_PIN = -1

if SDA_PIN < 0 or SCL_PIN < 0 then
  print("edit /root/lua/i2c_scan.lua first")
  print("set SDA_PIN and SCL_PIN to the external I2C pins you wired")
  return
end

i2c.open(SDA_PIN, SCL_PIN, { freq = 400000, pullup = true })
local found = i2c.scan()

if #found == 0 then
  print("no I2C devices found")
else
  print("I2C devices:")
  for _, addr in ipairs(found) do
    print(string.format("0x%02X", addr))
  end
end

i2c.close()
