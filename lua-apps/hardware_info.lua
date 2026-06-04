local breezy = require("breezy")

print("cwd:", breezy.cwd())
print("storage mounts:")
for i, m in ipairs(breezy.storage.mounts()) do
  local info = breezy.storage.info(m)
  print(i, m, info.type, "total=" .. info.total, "used=" .. info.used, "free=" .. info.free)
end

local net = breezy.network.info()
print("network connected:", net.connected)
if net.connected then
  print("ssid:", net.ssid)
  print("ip:", net.ip)
  print("gateway:", net.gateway)
  print("rssi:", net.rssi)
end

print("ADC example:")
print("  breezy.adc.read(1)")
print("Pin example:")
print('  breezy.pin.mode(13, "out"); breezy.pin.write(13, 1)')
print("Beep example:")
print("  breezy.sound.beep(4000, 50, 42)")
