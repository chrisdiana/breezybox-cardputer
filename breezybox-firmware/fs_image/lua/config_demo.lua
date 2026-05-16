local breezy = require("breezy")
local config = breezy.config

local path = "/root/demo_config.json"
local data = config.load(path)
data.runs = (data.runs or 0) + 1
data.last_run_ms = breezy.now_ms()
data.theme = data.theme or "amber"

config.save(data, path)

print("saved:", path)
print("runs:", data.runs)
print("last_run_ms:", data.last_run_ms)
print("theme:", config.get("theme", "none", path))
