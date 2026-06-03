local breezy = require("breezy")
local gfx = breezy.gfx

local function set_rainbow_palette()
  gfx.palette(0, 0, 0, 0)
  for i = 1, 255 do
    local h = math.floor((i - 1) * 1530 / 255)
    local seg = math.floor(h / 255)
    local t = h % 255
    local r, g, b
    if seg == 0 then
      r, g, b = 255, t, 0
    elseif seg == 1 then
      r, g, b = 255 - t, 255, 0
    elseif seg == 2 then
      r, g, b = 0, 255, t
    elseif seg == 3 then
      r, g, b = 0, 255 - t, 255
    elseif seg == 4 then
      r, g, b = t, 0, 255
    else
      r, g, b = 255, 0, 255 - t
    end
    gfx.palette(i, r, g, b)
  end
end

gfx.mode("150p")
set_rainbow_palette()

local w, h = gfx.size()
local x, y = 8, 8
local dx, dy = 3, 2
local bw, bh = 44, 28
local color = 1

for frame = 1, 180 do
  if frame == 1 then
    gfx.clear(0)
  end

  gfx.rect(x, y, bw, bh, color)
  gfx.rectfill(x + 10, y + 8, 10, 10, (color + 48) % 255)

  x = x + dx
  y = y + dy
  if x <= 0 or x + bw >= w then
    dx = -dx
    x = x + dx
  end
  if y <= 0 or y + bh >= h then
    dy = -dy
    y = y + dy
  end

  color = (color % 255) + 1
  gfx.wait_vsync()
  breezy.sleep(0.02)
end

gfx.mode("text")
print("graphics demo complete")
