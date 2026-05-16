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

local url = "http://postman-echo.com/post"
local body = "hello=breezy&mode=lua"

print("post:", url)

local res = net.http_post(url, body, "application/x-www-form-urlencoded")

print("status:", res.status or 0)
print("bytes:", #(res.body or ""))
if res.truncated ~= nil then
  print("truncated:", res.truncated and "yes" or "no")
end
print(res.body or "")
