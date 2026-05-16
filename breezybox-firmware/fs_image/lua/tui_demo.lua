local breezy = require("breezy")
local tui = breezy.tui

breezy.clear()
tui.cursor(false)
tui.box(1, 1, 40, 16, " Lua TUI Demo ")
tui.center("press arrows / hjkl / q", 3)
tui.write_at(3, 5, "status:", true)

local x = 20
local y = 9

local function draw()
  for row = 6, 14 do
    tui.write_at(3, row, string.rep(" ", 34))
  end
  tui.write_at(3, 5, "status: move the @ with arrows", true)
  tui.write_at(3, 7, "position: (" .. x .. ", " .. y .. ")", true)
  tui.write_at(x, y, "@")
  tui.status("q quit  hjkl move  arrows move")
end

draw()

while true do
  local key = breezy.readkey()
  if key == "q" then
    break
  elseif key == "h" or key == "\27[D" then
    x = math.max(3, x - 1)
  elseif key == "l" or key == "\27[C" then
    x = math.min(36, x + 1)
  elseif key == "k" or key == "\27[A" then
    y = math.max(6, y - 1)
  elseif key == "j" or key == "\27[B" then
    y = math.min(14, y + 1)
  else
    tui.status("key: " .. tostring(key))
  end
  draw()
end

tui.cursor(true)
breezy.clear()
print("tui demo done")
