local breezy = require("breezy")
local net = breezy.network

if not net.is_connected() then
  print("wifi not connected")
  print("run: wifi connect")
  return
end

local info = net.info()
print("ssid:", info.ssid or "")
print("ip:", info.ip or "")

local url = "http://example.com/"

print("head:", url)

local res = net.http_head(url)

print("status:", res.status or 0)
print("body bytes:", #(res.body or ""))
if res.truncated ~= nil then
  print("truncated:", res.truncated and "yes" or "no")
end
