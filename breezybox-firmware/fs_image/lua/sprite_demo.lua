local breezy = require("breezy")
local gfx = breezy.gfx

local old_backlight = gfx.backlight()
local img = gfx.new_image(12, 12, 0)

img:rectfill(2, 2, 8, 8, 12)
img:rect(1, 1, 10, 10, 15)
img:pixel(4, 4, 0)
img:pixel(7, 4, 0)
img:rectfill(4, 7, 4, 1, 1)

gfx.mode("150p")
gfx.backlight(220)
gfx.clear(1)
gfx.text(6, 6, "sprite demo", 15)

for x = 0, 70, 2 do
  gfx.rectfill(0, 20, 120, 30, 1)
  gfx.blit(img, 8 + x, 28, 0)
  gfx.blit_flip(img, 90 - x, 28, 0, true, false)
  gfx.wait_vsync()
  breezy.sleep_ms(20)
end

breezy.sleep_ms(300)
gfx.mode("text")
gfx.backlight(old_backlight)
print("done")
