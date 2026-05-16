local breezy = require("breezy")
local battery = breezy.battery

print("battery uv:", battery.read_uv())
print("battery pct:", battery.read_pct())
print("battery level:", battery.read_level())
